#pragma once

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

// =============================================================================
// ADCompressor DSP
//
// Signal flow (all stages driven by a single 0..1 "amount" value):
//
//   in ─┬─► analysis (undelayed): speech gate, input reference, S1 detector
//       │                     │ gain decisions 20 ms early
//       ├─► [20 ms delay] ─► × pre-gain ─► [S1 Leveler] ─► [S2 Snap] ─► [S3 Peak catch]
//       │                                                                  │
//       │                                          [Presence] ─► [De-esser]
//       │                                                                  │
//       │                                                     [Slow rider] ◄┘
//       │                                                                  │
//       │                  × makeup (predicted + trim + ride, ÷ pre-gain) ◄┘
//       │                                                                  │
//       │                                  slow trim ◄── [Look-ahead limiter]
//       │                                                                  │
//       └─► latency-matched dry ─────────────────────► bypass crossfade ◄──┘
//
// Input reference: the compressor thresholds are tuned for speech at about
// -18 LUFS. A slow pre-gain, driven by a running average of the input's
// speech-only loudness, brings any take to that reference before the
// compressors, so the knob behaves the same on hot or quiet recordings. Its
// exact inverse is applied at the makeup stage, so it is never heard.
//
// Makeup gain = predicted curve + slow trim.
//  * Predicted: because the compressors always see speech at the reference
//    level, the makeup each knob position needs is known in advance (measured
//    on real VO). Knob moves are compensated instantly, with nothing to learn.
//  * Trim: compares input loudness with the *final* output (after the
//    limiter, measured before the trim gain), both BS.1770-style (400 ms gated
//    blocks, ~2 min memory), i.e. the way a LUFS meter would, and glides toward
//    the gap. It absorbs voice-to-voice differences. The long memory matters:
//    a short one follows scene-to-scene changes and re-expands them (LRA).

//
// The learned reference and trim are saved with the plugin state, so a
// reopened session starts already calibrated.
// =============================================================================

namespace ad
{

inline float dbToGain (float db) noexcept { return std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g) noexcept  { return 20.0f * std::log10 (std::max (g, 1.0e-9f)); }
inline float powToDb (float p) noexcept   { return 10.0f * std::log10 (std::max (p, 1.0e-12f)); }

inline float timeCoef (float ms, double sampleRate) noexcept
{
    return (float) std::exp (-1.0 / (std::max (ms, 0.001f) * 0.001 * sampleRate));
}

// -----------------------------------------------------------------------------
// Allocation-free biquad (RBJ cookbook), transposed direct form II.
// -----------------------------------------------------------------------------
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;

    void reset() noexcept { z1 = z2 = 0; }

    float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void setHighShelf (double sr, double freq, double q, double gainDb) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w0 = juce::MathConstants<double>::twoPi * freq / sr;
        const double cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
        const double sA = 2.0 * std::sqrt (A) * alpha;

        const double nb0 = A * ((A + 1) + (A - 1) * cw + sA);
        const double nb1 = -2 * A * ((A - 1) + (A + 1) * cw);
        const double nb2 = A * ((A + 1) + (A - 1) * cw - sA);
        const double na0 = (A + 1) - (A - 1) * cw + sA;
        const double na1 = 2 * ((A - 1) - (A + 1) * cw);
        const double na2 = (A + 1) - (A - 1) * cw - sA;
        set (nb0, nb1, nb2, na0, na1, na2);
    }

    void setHighPass (double sr, double freq, double q) noexcept
    {
        const double w0 = juce::MathConstants<double>::twoPi * freq / sr;
        const double cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
        set ((1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
    }

private:
    void set (double nb0, double nb1, double nb2, double na0, double na1, double na2) noexcept
    {
        b0 = (float) (nb0 / na0); b1 = (float) (nb1 / na0); b2 = (float) (nb2 / na0);
        a1 = (float) (na1 / na0); a2 = (float) (na2 / na0);
    }
};

// K-weighting (ITU-R BS.1770) pre-filter: high shelf + high pass.
struct KWeighting
{
    Biquad shelf, hp;

    void prepare (double sr) noexcept
    {
        shelf.setHighShelf (sr, 1681.97, 0.7071, 3.9998);
        hp.setHighPass (sr, 38.135, 0.5003);
        reset();
    }
    void reset() noexcept { shelf.reset(); hp.reset(); }
    float process (float x) noexcept { return hp.process (shelf.process (x)); }
};

// -----------------------------------------------------------------------------
// Feed-forward compressor gain computer with soft knee and dB-domain ballistics.
// Stereo-linked: the caller passes one detector value per sample frame.
// -----------------------------------------------------------------------------
class CompressorStage
{
public:
    enum class Detector { peak, rms };

    void prepare (double sr) noexcept { sampleRate = sr; updateCoefs(); reset(); }
    void reset() noexcept { rmsEnv = 0; grDb = 0; }

    void setDetector (Detector d, float rmsWindowMs = 10.0f) noexcept
    {
        detector = d; rmsMs = rmsWindowMs; updateCoefs();
    }

    void setTimes (float attackMs, float releaseMs) noexcept
    {
        attMs = attackMs; relMs = releaseMs; updateCoefs();
    }

    void setCurve (float thresholdDb, float ratio, float kneeDb) noexcept
    {
        thr = thresholdDb;
        slope = 1.0f - 1.0f / std::max (ratio, 1.0f);
        knee = std::max (kneeDb, 0.0f);
    }

    // peakAbs: max |x| across channels. meanSquare: mean of x^2 across channels.
    float processFrame (float peakAbs, float meanSquare) noexcept
    {
        float levelDb;
        if (detector == Detector::rms)
        {
            rmsEnv = rmsCoef * rmsEnv + (1.0f - rmsCoef) * meanSquare;
            levelDb = powToDb (rmsEnv);
        }
        else
        {
            levelDb = gainToDb (peakAbs);
        }

        const float target = computeReductionDb (levelDb);
        const float c = target > grDb ? attCoef : relCoef;
        grDb = c * grDb + (1.0f - c) * target;
        return dbToGain (-grDb);
    }

    float getReductionDb() const noexcept { return grDb; }

private:
    float computeReductionDb (float x) const noexcept
    {
        if (slope <= 0.0f)
            return 0.0f;

        const float over = x - thr;
        if (knee > 0.0f && std::abs (over) <= knee * 0.5f)
        {
            const float t = over + knee * 0.5f;
            return slope * t * t / (2.0f * knee);
        }
        return over > 0.0f ? slope * over : 0.0f;
    }

    void updateCoefs() noexcept
    {
        attCoef = timeCoef (attMs, sampleRate);
        relCoef = timeCoef (relMs, sampleRate);
        rmsCoef = timeCoef (rmsMs, sampleRate);
    }

    double sampleRate = 48000.0;
    Detector detector = Detector::peak;
    float attMs = 10, relMs = 100, rmsMs = 10;
    float attCoef = 0, relCoef = 0, rmsCoef = 0;
    float thr = 0, slope = 0, knee = 0;
    float rmsEnv = 0, grDb = 0;
};

// -----------------------------------------------------------------------------
// Split-band de-esser. Only the band above the crossover is turned down, and
// only when it gets loud *relative to the full band*, so it follows the
// voice instead of needing an absolute threshold.
// -----------------------------------------------------------------------------
class DeEsser
{
public:
    void prepare (double sr, int numChannels, int maxBlock)
    {
        sampleRate = sr;
        xover.prepare ({ sr, (juce::uint32) maxBlock, (juce::uint32) numChannels });
        xover.setType (juce::dsp::LinkwitzRileyFilterType::lowpass);
        xover.setCutoffFrequency (6000.0f);
        envAtt = timeCoef (0.5f, sr);
        envRel = timeCoef (40.0f, sr);
        grAtt  = timeCoef (1.0f, sr);
        grRel  = timeCoef (50.0f, sr);
        lows.assign ((size_t) numChannels, 0.0f);
        highs.assign ((size_t) numChannels, 0.0f);
        reset();
    }

    void reset() noexcept { xover.reset(); hiEnv = fullEnv = 0; grDb = 0; }

    void setAmount (float a) noexcept { amount = a; }

    void processFrame (float* frame, int numChannels) noexcept
    {
        float hiPeak = 0, fullPeak = 0;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            float lo, hi;
            xover.processSample (ch, frame[ch], lo, hi);
            lows[(size_t) ch] = lo;
            highs[(size_t) ch] = hi;
            hiPeak = std::max (hiPeak, std::abs (hi));
            fullPeak = std::max (fullPeak, std::abs (frame[ch]));
        }

        follow (hiEnv, hiPeak);
        follow (fullEnv, fullPeak);

        const float hiDb = gainToDb (hiEnv);
        const float relDb = hiDb - gainToDb (fullEnv);
        float target = 0.0f;
        if (hiDb > -50.0f)
            target = amount * std::min (10.0f, 0.8f * std::max (0.0f, relDb - kRelThresholdDb));

        const float c = target > grDb ? grAtt : grRel;
        grDb = c * grDb + (1.0f - c) * target;

        const float g = dbToGain (-grDb);
        for (int ch = 0; ch < numChannels; ++ch)
            frame[ch] = lows[(size_t) ch] + highs[(size_t) ch] * g;
    }

    float getReductionDb() const noexcept { return grDb; }

private:
    static constexpr float kRelThresholdDb = -9.0f;

    void follow (float& env, float x) const noexcept
    {
        const float c = x > env ? envAtt : envRel;
        env = c * env + (1.0f - c) * x;
    }

    double sampleRate = 48000.0;
    juce::dsp::LinkwitzRileyFilter<float> xover;
    std::vector<float> lows, highs;
    float envAtt = 0, envRel = 0, grAtt = 0, grRel = 0;
    float hiEnv = 0, fullEnv = 0, grDb = 0, amount = 0;
};

// -----------------------------------------------------------------------------
// Look-ahead brickwall limiter (hold-min + release + box filter). The box
// filter spans the same window as the min-hold, which guarantees the gain has
// fully ramped down before a peak arrives — no overshoot.
// -----------------------------------------------------------------------------
class LookaheadLimiter
{
public:
    void prepare (double sr, int numChannels)
    {
        window = std::max (1, (int) std::round (0.0015 * sr));
        relCoef = timeCoef (60.0f, sr);
        required.assign ((size_t) window, 1.0f);
        smoothed.assign ((size_t) window, 1.0f);
        delay.assign ((size_t) numChannels, std::vector<float> ((size_t) window, 0.0f));
        reset();
    }

    void reset() noexcept
    {
        std::fill (required.begin(), required.end(), 1.0f);
        std::fill (smoothed.begin(), smoothed.end(), 1.0f);
        for (auto& d : delay) std::fill (d.begin(), d.end(), 0.0f);
        pos = 0; releaseGain = 1.0f; boxSum = (double) window; gainOut = 1.0f;
    }

    int getLatency() const noexcept { return window - 1; }

    void setCeilingDb (float db) noexcept { ceiling = dbToGain (db); }

    // Processes one frame in place (output is delayed by getLatency()).
    void processFrame (float* frame, int numChannels) noexcept
    {
        float peak = 0;
        for (int ch = 0; ch < numChannels; ++ch)
            peak = std::max (peak, std::abs (frame[ch]));

        required[(size_t) pos] = peak > ceiling ? ceiling / peak : 1.0f;

        float held = 1.0f;
        for (float r : required)
            held = std::min (held, r);

        if (held < releaseGain) releaseGain = held;
        else                    releaseGain = held + relCoef * (releaseGain - held);

        boxSum += (double) releaseGain - (double) smoothed[(size_t) pos];
        smoothed[(size_t) pos] = releaseGain;
        gainOut = (float) (boxSum / (double) window);

        const int readPos = (pos + 1) % window;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto& d = delay[(size_t) ch];
            d[(size_t) pos] = frame[ch];
            const float y = d[(size_t) readPos] * gainOut;
            frame[ch] = juce::jlimit (-ceiling, ceiling, y); // numerical safety only
        }

        pos = readPos;
    }

    float getReductionDb() const noexcept { return -gainToDb (gainOut); }

private:
    int window = 1, pos = 0;
    float ceiling = 1.0f, relCoef = 0, releaseGain = 1.0f, gainOut = 1.0f;
    double boxSum = 1.0;
    std::vector<float> required, smoothed;
    std::vector<std::vector<float>> delay;
};

// -----------------------------------------------------------------------------
// Running BS.1770-style loudness: 400 ms blocks every 100 ms, absolute gate at
// -70 LUFS, relative gate 10 LU below the abs-gated running mean (as in the
// standard, so a sudden drop in level can't lock the meter), exponentially averaged
// (in the power domain) over roughly the last few seconds of gated blocks.
// Feed it K-weighted power (sum over channels) one sample at a time.
// -----------------------------------------------------------------------------
class GatedLoudness
{
public:
    void prepare (double sr, double memorySeconds = 3.0) noexcept
    {
        hopLen = std::max (1, (int) std::round (0.1 * sr));
        memBlocks = (float) std::max (1.0, memorySeconds / 0.1); // memory, in 100 ms blocks
        reset();
    }

    void reset() noexcept
    {
        std::fill (std::begin (sub), std::end (sub), 0.0);
        acc = 0; count = 0; subIdx = 0; filled = 0;
        integratedPow = ungatedPow = 0; nGated = nUngated = 0; hasValue = hasUngated = false;
    }

    // Returns true when a new 400 ms block has completed.
    bool push (float kPow) noexcept
    {
        acc += kPow;
        if (++count < hopLen)
            return false;

        sub[subIdx] = acc / (double) hopLen;
        subIdx = (subIdx + 1) % 4;
        acc = 0; count = 0;
        filled = std::min (filled + 1, 4);
        if (filled < 4)
            return false;

        lastBlockPow = (float) ((sub[0] + sub[1] + sub[2] + sub[3]) * 0.25);
        const float blockLufs = powToDb (lastBlockPow) - 0.691f;
        const bool passAbs = blockLufs > -70.0f;
        if (! passAbs)
        {
            lastBlockGated = false;
            return true;
        }

        // Exact running average at first, becoming an EMA once the memory is
        // full, so a long memory isn't dominated by the first block.
        nUngated = std::min (nUngated + 1.0f, memBlocks);
        ungatedPow += (lastBlockPow - ungatedPow) / nUngated;
        hasUngated = true;
        lastBlockGated = blockLufs > powToDb (ungatedPow) - 0.691f - 10.0f;

        if (lastBlockGated)
        {
            nGated = std::min (nGated + 1.0f, memBlocks);
            integratedPow += (lastBlockPow - integratedPow) / nGated;
            hasValue = true;
        }
        return true;
    }

    bool valid() const noexcept { return hasValue; }
    float gatedSeconds() const noexcept { return nGated * 0.1f; } // capped at the memory length
    bool lastBlockPassed() const noexcept { return lastBlockGated; }
    float getLufs() const noexcept { return powToDb (integratedPow) - 0.691f; }

private:
    double sub[4] {};
    double acc = 0;
    int count = 0, hopLen = 4800, subIdx = 0, filled = 0;
    float memBlocks = 30.0f, nGated = 0, nUngated = 0, integratedPow = 0, ungatedPow = 0, lastBlockPow = 0;
    bool hasValue = false, hasUngated = false, lastBlockGated = false;
};

// -----------------------------------------------------------------------------
// The whole chain.
// -----------------------------------------------------------------------------
class VoiceChain
{
public:
    static constexpr int kMaxChannels = 2;
    static constexpr int kControlInterval = 32; // samples between parameter updates

    struct Meters
    {
        std::atomic<float> rideDb { 0 };     // slow rider gain (+ = lifting)
        std::atomic<float> compGrDb { 0 };   // S1 + S2 + S3
        std::atomic<float> deEssGrDb { 0 };
        std::atomic<float> limitGrDb { 0 };
        std::atomic<float> makeupDb { 0 };   // compression makeup (predicted + trim)
        std::atomic<float> inLoudDb { -100 };
        std::atomic<float> outLoudDb { -100 };
    };

    // Learned level calibration, saved with the plugin state so a reopened
    // session starts already matched instead of warming up again.
    struct Calibration
    {
        float inputLufs = -100.0f; // long-term speech loudness of the input
        float trimDb = 0.0f;
        float rideLufs = -100.0f;  // rider's long-term level (compressed, pre-makeup)
        bool valid = false;
    };

    void prepare (double sr, int maxBlock, int numChannels)
    {
        sampleRate = sr;
        channels = juce::jlimit (1, kMaxChannels, numChannels);

        s1.prepare (sr); s1.setDetector (CompressorStage::Detector::rms, 15.0f); s1.setTimes (15.0f, 180.0f);
        s2.prepare (sr); s2.setDetector (CompressorStage::Detector::peak);       s2.setTimes (3.0f, 60.0f);
        s3.prepare (sr); s3.setDetector (CompressorStage::Detector::peak);       s3.setTimes (0.3f, 50.0f);

        deEsser.prepare (sr, channels, maxBlock);
        limiter.prepare (sr, channels);
        limiter.setCeilingDb (kDefaultCeilingDb);

        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            kIn[ch].prepare (sr);
            kFinal[ch].prepare (sr);
            kMid[ch].prepare (sr);
            presence[ch].reset();
        }

        lookahead = std::max (1, (int) std::round (kLookaheadMs * 0.001 * sr));
        laBuf.assign ((size_t) kMaxChannels, std::vector<float> ((size_t) lookahead + 1, 0.0f));
        laPre.assign ((size_t) lookahead + 1, 1.0f);
        dryDelay.assign ((size_t) kMaxChannels, std::vector<float> ((size_t) getLatency() + 1, 0.0f));

        gateCoef          = timeCoef (50.0f, sr);   // fast input level for the speech gate
        engageCoef        = timeCoef (20.0f, sr);   // ramp when the reference first engages
        preGainCoefWarmup = timeCoef (300.0f, sr);  // reference glide while learning
        preGainCoef       = timeCoef (2000.0f, sr); // reference glide once settled
        minSpeechSamples  = 0.05 * sr; // speech needed before the reference engages
        engageRampSamples = 0.15 * sr; // speech over which the engage ramp runs
        warmupSamples     = 3.0 * sr;  // speech needed before the trim engages
        refMemorySamples  = kLongTermSeconds * sr; // long-term speech loudness
        rideShortCoef     = timeCoef (kRideDetectMs, sr);  // runs during active speech only
        rideGainCoef      = timeCoef (kRideGlideMs, sr);
        rideMinSamples    = kRideMinSpeechSeconds * sr;
        trimIn.prepare (sr, kLongTermSeconds);
        trimOut.prepare (sr, kLongTermSeconds);

        amountSmoothed.reset (sr, 0.05);
        ceilingSmoothed.reset (sr, 0.05);
        mix.reset (sr, 0.02);
        rideMix.reset (sr, 0.2);
        mix.setCurrentAndTargetValue (1.0f);
        firstBlock = true;
        lastPresenceDb = -1000.0f;
        reset();
    }

    void reset()
    {
        s1.reset(); s2.reset(); s3.reset();
        deEsser.reset(); limiter.reset();
        for (int ch = 0; ch < kMaxChannels; ++ch) { kIn[ch].reset(); kFinal[ch].reset(); kMid[ch].reset(); presence[ch].reset(); }
        for (auto& d : dryDelay) std::fill (d.begin(), d.end(), 0.0f);
        for (auto& d : laBuf) std::fill (d.begin(), d.end(), 0.0f);
        std::fill (laPre.begin(), laPre.end(), 1.0f);
        dryPos = laPos = 0;
        gatePow = 0;
        resetRider();
        trimIn.reset(); trimOut.reset();
        consumePendingCalibration();
        applyCalibration (learned);
    }

    int getLatency() const noexcept { return lookahead + limiter.getLatency(); }

    // Thread-safe: may be called from the message thread (state restore).
    void setCalibration (const Calibration& c) noexcept
    {
        pendingInputLufs.store (c.inputLufs);
        pendingTrimDb.store (c.trimDb);
        pendingRideLufs.store (c.rideLufs);
        pendingValid.store (c.valid);
        pendingFlag.store (true);
    }

    // Thread-safe snapshot of the latest learned calibration.
    Calibration getCalibration() const noexcept
    {
        return { publishedInputLufs.load(), publishedTrimDb.load(), publishedRideLufs.load(), publishedValid.load() };
    }

    // ceilingDb: limiter ceiling in dBFS (the plugin offers -1 or -6); changes glide over 50 ms.
    // rideOn: the rider's gain glides to 0 dB when off (it keeps learning, so it
    // picks up where it left off when switched back on).
    void process (juce::AudioBuffer<float>& buffer, float amountTarget, bool bypassed,
                  float ceilingDb = kDefaultCeilingDb, bool rideOn = true) noexcept
    {
        const int n = buffer.getNumSamples();
        const int nCh = std::min (channels, buffer.getNumChannels());
        if (nCh <= 0) return;

        if (consumePendingCalibration())
            applyCalibration (learned);

        amountTarget = juce::jlimit (0.0f, 1.0f, amountTarget);
        if (firstBlock)
        {
            amountSmoothed.setCurrentAndTargetValue (amountTarget);
            ceilingSmoothed.setCurrentAndTargetValue (ceilingDb);
            mix.setCurrentAndTargetValue (bypassed ? 0.0f : 1.0f);
            rideMix.setCurrentAndTargetValue (rideOn ? 1.0f : 0.0f);
            firstBlock = false;
        }
        amountSmoothed.setTargetValue (amountTarget);
        mix.setTargetValue (bypassed ? 0.0f : 1.0f);
        rideMix.setTargetValue (rideOn ? 1.0f : 0.0f);
        ceilingSmoothed.setTargetValue (ceilingDb);

        auto* const* data = buffer.getArrayOfWritePointers();
        const int dryLen = (int) dryDelay[0].size();

        float maxComp = 0, maxDs = 0, maxLim = 0;

        for (int start = 0; start < n; start += kControlInterval)
        {
            const int len = std::min (kControlInterval, n - start);
            const float a = amountSmoothed.skip (len);
            updateParameters (a);
            limiter.setCeilingDb (ceilingSmoothed.skip (len));

            for (int i = start; i < start + len; ++i)
            {
                float x[kMaxChannels] {}, y[kMaxChannels] {};
                float peak = 0, meanSq = 0, kInPow = 0;

                for (int ch = 0; ch < nCh; ++ch)
                {
                    x[ch] = data[ch][i];
                    const float k = kIn[ch].process (x[ch]);
                    kInPow += k * k;
                }

                // --- Speech detection & input reference ---------------------
                // The reference is a running mean of K-weighted input power
                // over speech only (pauses/room tone ignored). Early on it is
                // an exact average of everything heard so far, so a short
                // first word can't skew it; later it follows ~10 s of speech.
                gatePow = gateCoef * gatePow + (1.0f - gateCoef) * kInPow;
                const bool speech = gatePow > kGatePow;
                if (speech)
                {
                    speechSamples += 1.0;
                    refPow += ((double) kInPow - refPow) / std::min (speechSamples, refMemorySamples);
                }

                // Until the reference engages, nothing is applied (pre-gain 0,
                // makeup 0). It then ramps in over ~20 ms, so the first word
                // is never processed against a wrong level estimate.
                const bool engaged = speechSamples >= minSpeechSamples;
                if (engaged)
                {
                    const float target = juce::jlimit (-12.0f, 30.0f, kReferenceLufs - (powToDb ((float) refPow) - 0.691f));
                    const float c = speechSamples >= warmupSamples ? preGainCoef
                                  : speechSamples < minSpeechSamples + engageRampSamples ? engageCoef
                                  : preGainCoefWarmup;
                    preGainDb = c * preGainDb + (1.0f - c) * target;
                }
                const float makeupTarget = engaged ? predictedMakeupDb + trimDb : 0.0f;
                makeupDb = engageCoef * makeupDb + (1.0f - engageCoef) * makeupTarget;
                const float pre = dbToGain (preGainDb);


                // --- S1 leveler detects 20 ms ahead of the audio ------------
                for (int ch = 0; ch < nCh; ++ch)
                {
                    const float v = x[ch] * pre;
                    peak = std::max (peak, std::abs (v));
                    meanSq += v * v;
                }
                meanSq /= (float) nCh;
                const float g1 = s1.processFrame (peak, meanSq);

                // --- Lookahead delay (audio + the pre-gain it was given) ----
                const int laRead = (laPos + 1) % (lookahead + 1);
                laPre[(size_t) laPos] = pre;
                const float preD = laPre[(size_t) laRead];
                for (int ch = 0; ch < nCh; ++ch)
                {
                    auto& d = laBuf[(size_t) ch];
                    d[(size_t) laPos] = x[ch];
                    y[ch] = d[(size_t) laRead] * preD;
                }
                laPos = laRead;

                // --- Three compression stages (stereo-linked) ---------------
                // S2/S3 run without lookahead on purpose: their few-ms attack
                // is what lets consonant onsets through (the "snap").
                float p2 = 0;
                for (int ch = 0; ch < nCh; ++ch) { y[ch] *= g1; p2 = std::max (p2, std::abs (y[ch])); }

                const float g2 = s2.processFrame (p2, 0.0f);
                float p3 = 0;
                for (int ch = 0; ch < nCh; ++ch) { y[ch] *= g2; p3 = std::max (p3, std::abs (y[ch])); }

                const float g3 = s3.processFrame (p3, 0.0f);
                for (int ch = 0; ch < nCh; ++ch) y[ch] = presence[ch].process (y[ch] * g3);

                // --- De-esser ------------------------------------------------
                deEsser.processFrame (y, nCh);

                // --- Slow rider ---------------------------------------------
                // Evens out line-to-line level, like a fader rider. It sits
                // after the compressors (in front of them, they'd squash its
                // moves back out) and measures the compressed, K-weighted
                // level over the last ~200 ms of *active* speech only,
                // compares it with the long-term average measured the same
                // way, and glides the gain toward the difference. Between
                // lines it holds still, so gaps don't pump and room tone isn't
                // pulled up. Zero-mean by construction, so the loudness match
                // barely moves.
                {
                    float kMidPow = 0;
                    for (int ch = 0; ch < nCh; ++ch) { const float k = kMid[ch].process (y[ch]); kMidPow += k * k; }
                    rideFastPow = gateCoef * rideFastPow + (1.0f - gateCoef) * kMidPow;
                    const bool active = speech && engaged && (rideSamples < 1.0 || powToDb (rideFastPow) > powToDb ((float) rideLongPow) - kRideGateBelowDb);
                    if (active)
                    {
                        const double p = (double) kMidPow;
                        rideSamples += 1.0;
                        rideShortPow = rideSamples <= 1.0 ? p : rideShortCoef * rideShortPow + (1.0 - rideShortCoef) * p;
                        rideLongPow += (p - rideLongPow) / std::min (rideSamples, refMemorySamples);
                        if (rideSamples >= rideMinSamples)
                        {
                            const float diff = powToDb ((float) rideLongPow) - powToDb ((float) rideShortPow);
                            rideTargetDb = juce::jlimit (-kRideRangeDb, kRideRangeDb, rideStrength * diff);
                        }
                        rideDb = rideGainCoef * rideDb + (1.0f - rideGainCoef) * rideTargetDb;
                    }
                }

                // --- Makeup: predicted curve + slow trim + ride, minus pre-gain
                rideOutDb = rideDb * rideMix.getNextValue(); // Ride switch (glides 200 ms)
                const float makeup = dbToGain (makeupDb + rideOutDb) / preD; // exact inverse of the pre-gain
                for (int ch = 0; ch < nCh; ++ch) y[ch] *= makeup;

                // --- Brickwall limiter (adds latency) -----------------------
                limiter.processFrame (y, nCh);

                // --- Slow trim: compare final output with input (LUFS) -----
                // Input and output are each measured like a LUFS meter would
                // (independently gated, ~2 min memory, after the warm-up). The
                // output is measured *before* the trim gain, so the measurement
                // doesn't depend on the trim: once both meters have ~10 s of speech
                // (earlier estimates are too noisy), the trim glides (~8 s)
                // toward the measured gap, with no feedback loop to overshoot.
                // It only has to remove a small per-voice offset.
                float kFinalPow = 0;
                for (int ch = 0; ch < nCh; ++ch) { const float k = kFinal[ch].process (y[ch]); kFinalPow += k * k; }
                if (speechSamples >= warmupSamples)
                {
                    const float trimGain = dbToGain (trimDb);
                    const bool inBlock = trimIn.push (kInPow);
                    trimOut.push (kFinalPow / (trimGain * trimGain));
                    if (inBlock && trimIn.lastBlockPassed()
                        && trimIn.gatedSeconds() >= kTrimMinSeconds && trimOut.gatedSeconds() >= kTrimMinSeconds)
                    {
                        const float target = juce::jlimit (-6.0f, 6.0f, trimIn.getLufs() - trimOut.getLufs());
                        trimDb += kTrimStep * (target - trimDb);
                    }
                }

                // --- Latency-matched dry path & bypass crossfade ------------
                const float m = mix.getNextValue();
                const int readPos = (dryPos + 1) % dryLen;
                for (int ch = 0; ch < nCh; ++ch)
                {
                    auto& d = dryDelay[(size_t) ch];
                    d[(size_t) dryPos] = x[ch];
                    const float dry = d[(size_t) readPos];
                    data[ch][i] = dry + m * (y[ch] - dry);
                }
                dryPos = readPos;

                maxComp = std::max (maxComp, s1.getReductionDb() + s2.getReductionDb() + s3.getReductionDb());
                maxDs   = std::max (maxDs, deEsser.getReductionDb());
                maxLim  = std::max (maxLim, limiter.getReductionDb());
            }
        }

        // Remember what we've learned (survives reset() and is saved with the session).
        if (speechSamples >= warmupSamples)
        {
            learned = { powToDb ((float) refPow) - 0.691f, trimDb,
                        rideSamples >= rideMinSamples ? powToDb ((float) rideLongPow) - 0.691f : -100.0f, true };
            publishedInputLufs.store (learned.inputLufs);
            publishedTrimDb.store (learned.trimDb);
            publishedRideLufs.store (learned.rideLufs);
            publishedValid.store (true);
        }

        meters.rideDb.store (rideOutDb);
        meters.compGrDb.store (maxComp);
        meters.deEssGrDb.store (maxDs);
        meters.limitGrDb.store (maxLim);
        meters.makeupDb.store (predictedMakeupDb + trimDb);
        meters.inLoudDb.store (trimIn.valid() ? trimIn.getLufs() : -100.0f);
        meters.outLoudDb.store (trimOut.valid() ? trimOut.getLufs() + trimDb : -100.0f);
    }

    Meters meters;

private:
public:
    static constexpr float kDefaultCeilingDb = -1.0f;
private:
    static constexpr float kGatePow = 1.0e-5f; // ≈ -50 dB K-weighted
    static constexpr float kReferenceLufs = -18.0f;
    static constexpr float kTrimStep = 0.1f / 8.0f;   // per 100 ms block → ~8 s
    // Long memory on purpose: a short (10 s) match follows scene-to-scene level
    // changes and re-expands them, which raised LRA on real AD takes.
    static constexpr double kLongTermSeconds = 120.0; // reference & loudness-match memory
    static constexpr float kTrimMinSeconds = 10.0f;   // gated speech needed before the trim moves

    // Slow rider (line-to-line leveling).
    // Tuned on a 43-min AD episode against a commercial "input riding" compressor:
    // fast enough to act within a line (a slower rider only follows drift), and
    // the gain is held across gaps, so a line's quieter ending carries into the
    // next line's start (lifts short cues, keeps the first word's emphasis).
    static constexpr float kRideDetectMs = 200.0f;        // detector memory, in active speech
    static constexpr float kRideGlideMs = 80.0f;          // gain glide (only moves during speech)
    static constexpr float kRideRangeDb = 6.0f;           // max lift / cut
    static constexpr float kRideGateBelowDb = 15.0f;      // "active speech": within this of the reference
    static constexpr double kRideMinSpeechSeconds = 3.0;  // speech heard before the rider moves
    static constexpr float kRideMaxStrength = 1.0f;       // fraction of the level difference removed at 100%

    void resetRider() noexcept
    {
        rideShortPow = rideLongPow = 0; rideSamples = 0; rideFastPow = 0;
        rideDb = rideTargetDb = 0;
    }

    // Makeup the compression needs for speech at the -18 LUFS reference,
    // calibrated on real VO. The trim absorbs voice-to-voice differences.
    static float predictMakeupDb (float a) noexcept { return 8.5f * a + 2.7f * a * a; }

    bool consumePendingCalibration() noexcept
    {
        if (! pendingFlag.exchange (false))
            return false;
        learned = { pendingInputLufs.load(), pendingTrimDb.load(), pendingRideLufs.load(), pendingValid.load() };
        return true;
    }

    void applyCalibration (const Calibration& c) noexcept
    {
        if (c.valid)
        {
            // Start as if we'd already heard a few seconds of this voice.
            refPow = std::pow (10.0, (c.inputLufs + 0.691) / 10.0);
            speechSamples = warmupSamples;
            preGainDb = juce::jlimit (-12.0f, 30.0f, kReferenceLufs - c.inputLufs);
            trimDb = juce::jlimit (-6.0f, 6.0f, c.trimDb);
            makeupDb = predictedMakeupDb + trimDb;

            resetRider();
            if (c.rideLufs > -70.0f)
            {
                // Rider resumes at its learned level (as if it had heard 10 s).
                rideLongPow = rideShortPow = std::pow (10.0, (c.rideLufs + 0.691) / 10.0);
                rideSamples = std::max (rideMinSamples, 10.0 * sampleRate);
            }
        }
        else
        {
            refPow = 0; speechSamples = 0; preGainDb = 0; trimDb = 0; makeupDb = 0;
            resetRider();
        }
    }

    void updateParameters (float a) noexcept
    {
        // S1: slow RMS leveler — evens out phrase-to-phrase level.
        const float t1 = -14.0f - 14.0f * a;
        s1.setCurve (t1, 1.0f + 2.5f * a, 10.0f);

        // S2: snap compressor — 3 ms attack lets consonant transients through
        // before clamping down, fast release for an upfront, punchy sound.
        const float t2 = -8.0f - 14.0f * a;
        s2.setCurve (t2, 1.0f + 5.0f * a, 4.0f);

        // S3: fast peak catcher a few dB above S2 to tame the escaped spikes.
        s3.setCurve (t2 + 8.0f - 2.0f * a, a > 0.001f ? 20.0f : 1.0f, 2.0f);

        // Presence lift.
        const float presDb = 3.5f * a;
        if (std::abs (presDb - lastPresenceDb) > 0.01f)
        {
            for (auto& f : presence) f.setHighShelf (sampleRate, 4000.0, 0.7071, presDb);
            lastPresenceDb = presDb;
        }

        deEsser.setAmount (a);
        rideStrength = kRideMaxStrength * a;
        predictedMakeupDb = predictMakeupDb (a);
    }

    double sampleRate = 48000.0;
    int channels = 2;

    CompressorStage s1, s2, s3;
    DeEsser deEsser;
    LookaheadLimiter limiter;
    Biquad presence[kMaxChannels];
    KWeighting kIn[kMaxChannels], kFinal[kMaxChannels], kMid[kMaxChannels];

    static constexpr double kLookaheadMs = 20.0;
    std::vector<std::vector<float>> dryDelay, laBuf;
    std::vector<float> laPre;
    int dryPos = 0, laPos = 0, lookahead = 1;

    float gateCoef = 0, gatePow = 0;
    float preGainCoef = 0, preGainCoefWarmup = 0, preGainDb = 0;
    double refPow = 0; // long-term speech power (double: the 120 s average has tiny per-sample steps)
    float trimDb = 0, predictedMakeupDb = 0, makeupDb = 0, engageCoef = 0;
    double speechSamples = 0, minSpeechSamples = 0, engageRampSamples = 0, warmupSamples = 0, refMemorySamples = 1;
    GatedLoudness trimIn, trimOut;

    double rideShortPow = 0, rideLongPow = 0, rideSamples = 0, rideMinSamples = 1;
    float rideFastPow = 0, rideShortCoef = 0, rideGainCoef = 0, rideDb = 0, rideTargetDb = 0, rideStrength = 0, rideOutDb = 0;

    Calibration learned;
    std::atomic<float> pendingInputLufs { -100.0f }, pendingTrimDb { 0.0f }, pendingRideLufs { -100.0f };
    std::atomic<bool> pendingValid { false }, pendingFlag { false };
    std::atomic<float> publishedInputLufs { -100.0f }, publishedTrimDb { 0.0f }, publishedRideLufs { -100.0f };
    std::atomic<bool> publishedValid { false };

    bool firstBlock = true;
    float lastPresenceDb = -1000.0f;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> amountSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> ceilingSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mix;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> rideMix; // Ride switch
};

} // namespace ad
