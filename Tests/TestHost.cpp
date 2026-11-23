// Headless tests for ADCompressor (run by ctest on every platform in CI).
// Drives the plugin's processor directly (no plugin host needed) with
// synthetic voice-like material and checks: loudness match (BS.1770-style
// gated), the -1 dBFS output ceiling, dynamic spread reduction, loudness
// range (LRA) reduction, bit-exact latency-compensated bypass, and
// saved-calibration restore.
//
// Usage:
//   ADCompressorTest                       run the test suite
//   ADCompressorTest --render take.wav     render a take at 0/50/100% next to it
//   ADCompressorTest --calibrate take.wav  print steady-state makeup per knob position
//   ADCompressorTest --trace [sampleRate]  print the learned reference/trim over time

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "PluginProcessor.h"
#include <iostream>
#include <random>

namespace
{
struct Loudness
{
    double integrated = -100, spread = 0, floor = -100;
};

Loudness measure (const juce::AudioBuffer<float>& b, double sr, int startSample)
{
    const int nCh = b.getNumChannels();
    std::vector<ad::KWeighting> k ((size_t) nCh);
    for (auto& f : k) f.prepare (sr);

    std::vector<double> sq ((size_t) b.getNumSamples(), 0.0);
    for (int ch = 0; ch < nCh; ++ch)
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const double v = k[(size_t) ch].process (b.getSample (ch, i));
            sq[(size_t) i] += v * v;
        }

    const int blk = (int) (0.4 * sr), hop = (int) (0.1 * sr);
    std::vector<double> blocks, all;
    for (int s = startSample; s + blk <= b.getNumSamples(); s += hop)
    {
        double acc = 0;
        for (int i = s; i < s + blk; ++i) acc += sq[(size_t) i];
        const double l = -0.691 + 10.0 * std::log10 (std::max (acc / blk, 1e-12));
        all.push_back (l);
        if (l > -70.0) blocks.push_back (l);
    }
    auto energyMean = [] (const std::vector<double>& v)
    {
        double e = 0; for (double l : v) e += std::pow (10.0, l / 10.0);
        return v.empty() ? -100.0 : 10.0 * std::log10 (e / (double) v.size());
    };
    const double ungated = energyMean (blocks);
    std::vector<double> gated;
    for (double l : blocks) if (l > ungated - 10.0) gated.push_back (l);

    Loudness r;
    r.integrated = energyMean (gated);
    double m = 0; for (double l : gated) m += l; m /= std::max<size_t> (1, gated.size());
    double v = 0; for (double l : gated) v += (l - m) * (l - m);
    r.spread = std::sqrt (v / std::max<size_t> (1, gated.size()));
    if (! all.empty())
    {
        // Noise floor: 5th percentile of 400 ms blocks (pauses / room tone).
        std::sort (all.begin(), all.end());
        r.floor = all[(size_t) (0.05 * (double) (all.size() - 1))];
    }
    return r;
}

// EBU R128 loudness range: 3 s windows every 100 ms, -70 LUFS absolute and
// -20 LU relative gate, 95th minus 10th percentile.
double loudnessRange (const juce::AudioBuffer<float>& b, double sr, int startSample)
{
    const int nCh = b.getNumChannels();
    std::vector<ad::KWeighting> k ((size_t) nCh);
    for (auto& f : k) f.prepare (sr);
    const int hop = (int) (0.1 * sr);
    std::vector<double> hops;
    double acc = 0; int cnt = 0;
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        for (int ch = 0; ch < nCh; ++ch) { const double v = k[(size_t) ch].process (b.getSample (ch, i)); if (i >= startSample) acc += v * v; }
        if (i >= startSample && ++cnt == hop) { hops.push_back (acc / hop); acc = 0; cnt = 0; }
    }
    std::vector<double> st;
    for (size_t h = 30; h <= hops.size(); ++h)
    {
        double e = 0; for (size_t j = h - 30; j < h; ++j) e += hops[j];
        const double l = -0.691 + 10.0 * std::log10 (std::max (e / 30.0, 1e-12));
        if (l > -70.0) st.push_back (l);
    }
    if (st.empty()) return 0;
    double e = 0; for (double l : st) e += std::pow (10.0, l / 10.0);
    const double rel = 10.0 * std::log10 (e / (double) st.size()) - 20.0;
    std::vector<double> g; for (double l : st) if (l > rel) g.push_back (l);
    std::sort (g.begin(), g.end());
    auto pct = [&g] (double p) { return g[(size_t) (p * (double) (g.size() - 1))]; };
    return pct (0.95) - pct (0.10);
}

float peakDb (const juce::AudioBuffer<float>& b, int start)
{
    float p = 0;
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        p = std::max (p, b.getMagnitude (ch, start, b.getNumSamples() - start));
    return ad::gainToDb (p);
}

// Voice-ish test signal: voiced "syllables" (harmonic, gliding f0) at random
// levels, sibilant noise bursts and pauses. Level offset applied in dB.
juce::AudioBuffer<float> makeVoice (double sr, double seconds, int nCh, float levelDb, unsigned seed)
{
    const int n = (int) (seconds * sr);
    juce::AudioBuffer<float> b (nCh, n);
    b.clear();
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> uni (0.0f, 1.0f);
    std::normal_distribution<float> gauss (0.0f, 1.0f);

    ad::Biquad sibHp; sibHp.setHighPass (sr, 5500.0, 0.7);
    ad::Biquad tilt;  tilt.setHighShelf (sr, 1500.0, 0.7, -10.0); // voice spectral tilt

    int i = 0;
    double phase = 0;
    while (i < n)
    {
        const bool pause = uni (rng) < 0.18f;
        const bool sibilant = ! pause && uni (rng) < 0.25f;
        const int len = (int) (sr * (pause ? 0.08 + 0.3 * uni (rng) : 0.07 + 0.22 * uni (rng)));
        const float lvl = ad::dbToGain (levelDb + (uni (rng) * 18.0f - 12.0f));
        const float f0 = 110.0f + 70.0f * uni (rng);

        for (int j = 0; j < len && i < n; ++j, ++i)
        {
            const float t = (float) j / (float) len;
            // Fast attack (consonant onset), decaying body.
            const float env = std::min (1.0f, t * 40.0f) * std::pow (1.0f - t, 0.6f);
            float s = 0.0f;
            if (pause)
                s = 0.0005f * gauss (rng); // room tone
            else if (sibilant)
                s = lvl * 0.9f * env * sibHp.process (gauss (rng));
            else
            {
                phase += juce::MathConstants<double>::twoPi * (f0 * (1.0 + 0.1 * t)) / sr;
                for (int h = 1; h <= 30; ++h)
                    s += (float) std::sin (phase * h) / (float) h;
                s = lvl * env * 0.5f * tilt.process (s);
            }
            for (int ch = 0; ch < nCh; ++ch)
                b.setSample (ch, i, s);
        }
    }
    return b;
}



juce::AudioProcessorParameter* findParam (juce::AudioProcessor& p, const juce::String& name)
{
    for (auto* prm : p.getParameters())
        if (prm->getName (64) == name) return prm;
    return nullptr;
}

std::unique_ptr<juce::AudioProcessor> load (double sr, int block, int nCh)
{
    auto inst = std::make_unique<ADCompressorProcessor>();

    auto layout = nCh == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
    juce::AudioProcessor::BusesLayout bl;
    bl.inputBuses.add (layout); bl.outputBuses.add (layout);
    if (! inst->setBusesLayout (bl)) { std::cerr << "Layout rejected\n"; std::exit (1); }

    inst->setRateAndBufferSizeDetails (sr, block);
    inst->prepareToPlay (sr, block);
    return std::unique_ptr<juce::AudioProcessor> (inst.release());
}

juce::AudioBuffer<float> run (juce::AudioProcessor& p, const juce::AudioBuffer<float>& in, int block)
{
    juce::AudioBuffer<float> out (in);
    juce::MidiBuffer midi;
    for (int s = 0; s < out.getNumSamples(); s += block)
    {
        const int len = std::min (block, out.getNumSamples() - s);
        juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), out.getNumChannels(), s, len);
        p.processBlock (view, midi);
    }
    return out;
}

int failures = 0;
void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  [PASS] " : "  [FAIL] ") << what << "\n";
    if (! ok) ++failures;
}
} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;

    struct Config { double sr; int nCh; int block; };
    // --calibrate <wav>: run the DSP directly and print the steady-state
    // compression makeup (excluding the input reference) per knob position.
    if (argc >= 3 && juce::String (argv[1]) == "--calibrate")
    {
        juce::AudioFormatManager afm; afm.registerBasicFormats();
        std::vector<std::pair<juce::String, juce::AudioBuffer<float>>> sources;
        sources.push_back ({ "synthetic", makeVoice (48000.0, 60.0, 1, -20.0f, 7) });
        std::unique_ptr<juce::AudioFormatReader> reader (afm.createReaderFor (juce::File (argv[2])));
        if (reader && reader->sampleRate == 48000.0)
        {
            juce::AudioBuffer<float> b (1, (int) reader->lengthInSamples);
            reader->read (&b, 0, b.getNumSamples(), 0, true, false);
            sources.push_back ({ "take", std::move (b) });
        }
        for (int step = 0; step <= 10; ++step)
        {
            std::cout << "a=" << juce::String (step / 10.0, 1);
            for (auto& [name, src] : sources)
            {
                ad::VoiceChain chain;
                chain.prepare (48000.0, 512, 1);
                juce::AudioBuffer<float> buf (src);
                double sum = 0; int cnt = 0;
                for (int s = 0; s < buf.getNumSamples(); s += 512)
                {
                    const int len = std::min (512, buf.getNumSamples() - s);
                    juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), 1, s, len);
                    chain.process (view, (float) step / 10.0f, false);
                    if (s > 20 * 48000) { sum += chain.meters.makeupDb.load(); ++cnt; }
                }
                std::cout << "  " << name << " " << juce::String (sum / cnt, 2);
            }
            std::cout << "\n";
        }
        return 0;
    }

    if (argc >= 2 && juce::String (argv[1]) == "--trace")
    {
        const double sr = argc >= 3 ? juce::String (argv[2]).getDoubleValue() : 48000.0;
        for (float lvl : { -14.0f, -24.0f })
        {
            auto src = makeVoice (sr, 40.0, 1, lvl, 99);
            ad::VoiceChain chain; chain.prepare (sr, 512, 1);
            std::cout << "level " << lvl << "\n";
            for (int s = 0; s < src.getNumSamples(); s += 512)
            {
                juce::AudioBuffer<float> view (src.getArrayOfWritePointers(), 1, s, std::min (512, src.getNumSamples() - s));
                chain.process (view, 0.7f, false);
                if ((s / 512) % (int) (4.0 * sr / 512) == 0)
                {
                    auto c = chain.getCalibration();
                    std::cout << "  t=" << juce::String (s / sr, 1) << " ref " << juce::String (c.inputLufs, 2)
                              << " trim " << juce::String (c.trimDb, 2) << " inLUFS " << juce::String (chain.meters.inLoudDb.load(), 2)
                              << " outLUFS " << juce::String (chain.meters.outLoudDb.load(), 2) << "\n";
                }
            }
        }
        return 0;
    }

    const bool renderOnly = argc >= 3 && juce::String (argv[1]) == "--render";
    if (! renderOnly)
    for (auto cfg : { Config { 48000.0, 2, 512 }, Config { 44100.0, 1, 128 }, Config { 96000.0, 2, 1024 } })
    {
        std::cout << "\n=== " << cfg.sr << " Hz, " << cfg.nCh << " ch, block " << cfg.block << " ===\n";
        // The loudness match is long-term (like an integrated LUFS reading
        // over a whole take), so test on a long take after a short warm-up.
        const auto input = makeVoice (cfg.sr, 150.0, cfg.nCh, -14.0f, 1234);
        const int settle = (int) (15.0 * cfg.sr); // warm-up excluded
        const auto inL = measure (input, cfg.sr, settle);
        std::cout << "  input: " << juce::String (inL.integrated, 2) << " LUFS, spread "
                  << juce::String (inL.spread, 2) << " LU, peak " << juce::String (peakDb (input, 0), 2) << " dBFS\n";

        for (float amount : { 0.0f, 25.0f, 50.0f, 75.0f, 100.0f })
        {
            auto inst = load (cfg.sr, cfg.block, cfg.nCh);
            if (! inst) { std::cerr << "Could not load plugin\n"; return 1; }
            auto* amt = findParam (*inst, "Amount");
            if (! amt) { std::cerr << "No Amount param\n"; return 1; }
            amt->setValueNotifyingHost (amount / 100.0f);

            const auto out = run (*inst, input, cfg.block);
            const auto outL = measure (out, cfg.sr, settle);
            const float pk = peakDb (out, 0);
            std::cout << "  amount " << juce::String (amount, 0).paddedLeft (' ', 3) << "%: "
                      << juce::String (outL.integrated, 2) << " LUFS (diff "
                      << juce::String (outL.integrated - inL.integrated, 2) << " dB), spread "
                      << juce::String (outL.spread, 2) << " LU, peak " << juce::String (pk, 2)
                      << " dBFS, latency " << inst->getLatencySamples() << "\n";

            check (std::abs (outL.integrated - inL.integrated) < 1.0, "loudness matched within 1 dB");
            check (pk <= -0.99f, "output peak <= -1 dBFS ceiling");
            if (amount >= 100.0f)
                check (outL.spread < inL.spread * 0.85, "dynamic spread reduced at full amount");
        }

        // Bypass must be a pure latency-compensated passthrough.
        {
            auto inst = load (cfg.sr, cfg.block, cfg.nCh);
            auto* bp = inst->getBypassParameter();
            check (bp != nullptr, "plugin exposes a host bypass parameter");
            if (bp != nullptr)
            {
                bp->setValueNotifyingHost (1.0f);
                const auto out = run (*inst, input, cfg.block);
                const int lat = inst->getLatencySamples();
                float maxErr = 0;
                for (int ch = 0; ch < cfg.nCh; ++ch)
                    for (int i = lat + cfg.block * 4; i < input.getNumSamples(); ++i)
                        maxErr = std::max (maxErr, std::abs (out.getSample (ch, i) - input.getSample (ch, i - lat)));
                check (maxErr < 1.0e-6f, "bypass is latency-aligned passthrough (max err "
                                         + juce::String (maxErr, 9) + ")");
            }
        }

        // Ceiling switch at -6 dBFS: no output sample may exceed it.
        {
            auto inst = load (cfg.sr, cfg.block, cfg.nCh);
            findParam (*inst, "Amount")->setValueNotifyingHost (1.0f);
            auto* ceiling = findParam (*inst, "Ceiling");
            check (ceiling != nullptr, "plugin exposes a Ceiling parameter");
            if (ceiling != nullptr)
            {
                ceiling->setValueNotifyingHost (1.0f); // -6 dBFS
                const auto out = run (*inst, input, cfg.block);
                const float pk = peakDb (out, 0);
                std::cout << "  ceiling -6 dBFS @100%: peak " << juce::String (pk, 2) << " dBFS\n";
                check (pk <= -5.99f, "output peak <= -6 dBFS with the -6 ceiling selected");
            }
        }

        // Saved calibration: a reloaded instance should start exactly where the
        // session left off, with no warm-up. Its first 5 s must sound like
        // the same 5 s through the fully warmed-up instance.
        {
            auto first = load (cfg.sr, cfg.block, cfg.nCh);
            findParam (*first, "Amount")->setValueNotifyingHost (0.8f);
            run (*first, input, cfg.block);
            juce::MemoryBlock state;
            first->getStateInformation (state);

            auto second = load (cfg.sr, cfg.block, cfg.nCh);
            second->setStateInformation (state.getData(), (int) state.getSize());
            second->prepareToPlay (cfg.sr, cfg.block);
            juce::AudioBuffer<float> head (cfg.nCh, (int) (5.0 * cfg.sr));
            for (int ch = 0; ch < cfg.nCh; ++ch) head.copyFrom (ch, 0, input, ch, 0, head.getNumSamples());
            const auto hRestored = measure (run (*second, head, cfg.block), cfg.sr, 0);
            const auto hWarm = measure (run (*first, head, cfg.block), cfg.sr, 0); // continues, already warm
            std::cout << "  first 5 s: restored session " << juce::String (hRestored.integrated, 2)
                      << " / warmed-up " << juce::String (hWarm.integrated, 2) << " LUFS\n";
            check (std::abs (hRestored.integrated - hWarm.integrated) < 1.0,
                   "restored session starts within 1 dB of a warmed-up one");
        }

        // Level jump: a 10 dB quieter take should still come out matched.
        {
            const auto quiet = makeVoice (cfg.sr, 150.0, cfg.nCh, -24.0f, 99);
            auto inst = load (cfg.sr, cfg.block, cfg.nCh);
            findParam (*inst, "Amount")->setValueNotifyingHost (0.7f);
            const auto qIn = measure (quiet, cfg.sr, settle);
            const auto qOut = measure (run (*inst, quiet, cfg.block), cfg.sr, settle);
            std::cout << "  quiet take @70%: in " << juce::String (qIn.integrated, 2) << " / out "
                      << juce::String (qOut.integrated, 2) << " LUFS\n";
            check (std::abs (qOut.integrated - qIn.integrated) < 1.0, "quiet take loudness matched within 1 dB");
            check (qOut.spread < qIn.spread * 0.9, "quiet take still gets compressed (input reference)");
        }

        // Scene-level changes (quiet and loud stretches, 25 s each): full amount
        // must lower the loudness range, not follow the changes back up.
        {
            const int sec = (int) (25.0 * cfg.sr);
            juce::AudioBuffer<float> scenes (cfg.nCh, sec * 6);
            for (int part = 0; part < 6; ++part)
            {
                const auto v = makeVoice (cfg.sr, 25.0, cfg.nCh, (part % 2) ? -20.0f : -14.0f, 500u + (unsigned) part);
                for (int ch = 0; ch < cfg.nCh; ++ch) scenes.copyFrom (ch, part * sec, v, ch, 0, sec);
            }
            auto inst = load (cfg.sr, cfg.block, cfg.nCh);
            findParam (*inst, "Amount")->setValueNotifyingHost (1.0f);
            const double lraIn = loudnessRange (scenes, cfg.sr, settle);
            const double lraOut = loudnessRange (run (*inst, scenes, cfg.block), cfg.sr, settle);
            std::cout << "  scene changes @100%: LRA " << juce::String (lraIn, 1) << " -> " << juce::String (lraOut, 1) << " LU\n";
            check (lraOut < lraIn * 0.5, "full amount at least halves loudness range (LRA) across scene changes");
        }
    }

    // --render: process a real file at 0/50/100%.
    if (renderOnly)
    {
        juce::File wav (argv[2]);
        juce::AudioFormatManager afm; afm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (afm.createReaderFor (wav));
        if (reader)
        {
            const int nCh = (int) std::min<unsigned> (2, reader->numChannels);
            juce::AudioBuffer<float> buf (nCh, (int) reader->lengthInSamples);
            reader->read (&buf, 0, buf.getNumSamples(), 0, true, nCh > 1);
            const double sr = reader->sampleRate;
            const auto inL = measure (buf, sr, 0);
            const float inPk = peakDb (buf, 0);
            std::cout << "Input: " << juce::String (inL.integrated, 2) << " LUFS, peak " << juce::String (inPk, 2)
                      << " dBFS, PLR " << juce::String (inPk - inL.integrated, 1) << " dB, spread "
                      << juce::String (inL.spread, 2) << " LU, floor " << juce::String (inL.floor, 1) << " LUFS\n";
            for (float amount : { 0.0f, 50.0f, 100.0f })
            {
                auto inst = load (sr, 512, nCh);
                findParam (*inst, "Amount")->setValueNotifyingHost (amount / 100.0f);
                auto out = run (*inst, buf, 512);
                const auto outL = measure (out, sr, 0);
                auto dest = wav.getSiblingFile (wav.getFileNameWithoutExtension() + "_AD"
                                                + juce::String ((int) amount) + ".wav");
                dest.deleteFile();
                juce::WavAudioFormat wf;
                std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (dest);
                auto w = wf.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                         .withSampleRate (sr)
                                                         .withNumChannels (nCh)
                                                         .withBitsPerSample (24));
                if (w) w->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
                const float pk = peakDb (out, 0);
                std::cout << "  " << juce::String ((int) amount).paddedLeft (' ', 3) << "%: "
                          << juce::String (outL.integrated, 2) << " LUFS (diff "
                          << juce::String (outL.integrated - inL.integrated, 2) << "), peak " << juce::String (pk, 2)
                          << " dBFS, PLR " << juce::String (pk - outL.integrated, 1) << " dB, spread "
                          << juce::String (outL.spread, 2) << " LU, floor " << juce::String (outL.floor, 1)
                          << " LUFS  -> " << dest.getFileName() << "\n";
            }
        }
    }

    std::cout << "\n" << (failures == 0 ? "ALL TESTS PASSED" : juce::String (failures) + " FAILURE(S)") << "\n";
    return failures == 0 ? 0 : 1;
}
