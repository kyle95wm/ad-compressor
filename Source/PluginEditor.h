#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "BrandLookAndFeel.h"
#include "UpdateFooter.h"

class ADCompressorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit ADCompressorEditor (ADCompressorProcessor&);
    ~ADCompressorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void drawMeter (juce::Graphics&, juce::Rectangle<float> area, const juce::String& label,
                    float grDb, float rangeDb, juce::Colour colour);
    void drawRideMeter (juce::Graphics&, juce::Rectangle<float> area, float rideDb, float rangeDb);

    ADCompressorProcessor& audioProcessor;
    brand::LookAndFeel lnf;

    juce::Slider knob;
    juce::ToggleButton bypassButton { "Bypass" };
    juce::ToggleButton ceilingButton { "Ceiling" };

    UpdateFooter footer;

    juce::AudioProcessorValueTreeState::SliderAttachment knobAttachment;
    juce::AudioProcessorValueTreeState::ButtonAttachment bypassAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> ceilingAttachment;

    float ride = 0, compGr = 0, deEssGr = 0, limitGr = 0, makeup = 0;
    juce::Rectangle<float> meterArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ADCompressorEditor)
};
