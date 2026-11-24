#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "VoiceChain.h"
#include "UpdateChecker.h"

class ADCompressorProcessor : public juce::AudioProcessor
{
public:
    ADCompressorProcessor();
    ~ADCompressorProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParam; }

    juce::AudioProcessorValueTreeState apvts;
    UpdateChecker updateChecker;
    const ad::VoiceChain::Meters& getMeters() const noexcept { return chain.meters; }

    static constexpr const char* amountId = "amount";
    static constexpr const char* bypassId = "bypass";
    static constexpr const char* ceilingId = "ceiling"; // 0 = -1 dBFS, 1 = -6 dBFS
    static constexpr const char* rideId = "ride";       // slow rider on/off (added in 0.3.0)

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    ad::VoiceChain chain;
    std::atomic<float>* amountParam = nullptr;
    juce::AudioParameterBool* bypassParam = nullptr;
    juce::AudioParameterChoice* ceilingParam = nullptr;
    juce::AudioParameterBool* rideParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ADCompressorProcessor)
};
