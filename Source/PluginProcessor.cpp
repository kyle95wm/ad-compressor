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
    jassert (amountParam != nullptr && bypassParam != nullptr);
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

    chain.process (buffer, amountParam->load() / 100.0f, bypassParam->get());
}

void ADCompressorProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    // Save the learned level calibration so the session reopens already matched.
    const auto cal = chain.getCalibration();
    if (cal.valid)
    {
        state.setProperty ("calInputLufs", cal.inputLufs, nullptr);
        state.setProperty ("calTrimDb", cal.trimDb, nullptr);
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
