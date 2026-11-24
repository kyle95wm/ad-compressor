#include "PluginProcessor.h"
#include "PluginEditor.h"

ADCompressorProcessor::ADCompressorProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "ADCompressor", createLayout())
{
    amountParam = apvts.getRawParameterValue (amountId);
    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (bypassId));
    ceilingParam = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (ceilingId));
    rideParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (rideId));
    jassert (amountParam != nullptr && bypassParam != nullptr && ceilingParam != nullptr && rideParam != nullptr);
}

juce::AudioProcessorValueTreeState::ParameterLayout ADCompressorProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { amountId, 1 }, "Amount",
        juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel ("%")
            .withStringFromValueFunction ([] (float v, int) { return juce::String (v, 0) + "%"; })));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { bypassId, 1 }, "Bypass", false));

    // Output ceiling. -1 dBFS suits any recording level; -6 dBFS suits takes
    // recorded with headroom (on hot takes it limits consonants audibly).
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { ceilingId, 1 }, "Ceiling", juce::StringArray { "-1 dBFS", "-6 dBFS" }, 0));

    // Slow rider on/off. Defaults to on, so sessions saved before 0.3 sound the same.
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { rideId, 2 }, "Ride", true));

    return layout;
}

bool ADCompressorProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return out == layouts.getMainInputChannelSet();
}

void ADCompressorProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    chain.prepare (sampleRate, samplesPerBlock, getTotalNumOutputChannels());
    setLatencySamples (chain.getLatency());
}

void ADCompressorProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    chain.process (buffer, amountParam->load() / 100.0f, bypassParam->get(),
                   ceilingParam->getIndex() == 1 ? -6.0f : -1.0f, rideParam->get());
}

void ADCompressorProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    // Save the learned level calibration so the session reopens already matched.
    // Drop whatever a restored state carried first: after a Relearn (or before
    // the warm-up) there is nothing valid to save, and the old values must not
    // come back on the next reopen.
    for (auto* key : { "calInputLufs", "calTrimDb", "calRideLufs", "calRideLufs2", "calMatched" })
        state.removeProperty (key, nullptr);
    const auto cal = chain.getCalibration();
    if (cal.valid)
    {
        state.setProperty ("calInputLufs", cal.inputLufs, nullptr);
        state.setProperty ("calTrimDb", cal.trimDb, nullptr);
        state.setProperty ("calRideLufs2", cal.rideLufs, nullptr); // includes predicted makeup (0.4+)
        state.setProperty ("calMatched", cal.matched, nullptr);    // 0.5+
    }

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void ADCompressorProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto state = juce::ValueTree::fromXml (*xml);
            if (state.hasProperty ("calInputLufs"))
            {
                ad::VoiceChain::Calibration cal;
                cal.inputLufs = (float) state.getProperty ("calInputLufs");
                cal.trimDb = (float) state.getProperty ("calTrimDb", 0.0f);
                // "calRideLufs" (0.2-0.3) was measured without the predicted makeup, so it does not
                // apply any more; without the new key the rider relearns (3 s of speech).
                cal.rideLufs = (float) state.getProperty ("calRideLufs2", -100.0f);
                // Absent before 0.5: those sessions were saved after real playback, so
                // treat them as calibrated rather than showing "matching" again.
                cal.matched = (bool) state.getProperty ("calMatched", true);
                cal.valid = std::isfinite (cal.inputLufs) && cal.inputLufs > -70.0f;
                chain.setCalibration (cal);
            }
            apvts.replaceState (state);
        }
}

juce::AudioProcessorEditor* ADCompressorProcessor::createEditor()
{
    return new ADCompressorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ADCompressorProcessor();
}
