#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "UpdateChecker.h"

class KnobLookAndFeel : public juce::LookAndFeel_V4
{
public:
    KnobLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
};

class ADCompressorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit ADCompressorEditor (ADCompressorProcessor&);
    ~ADCompressorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void refreshUpdateUi();
    void drawMeter (juce::Graphics&, juce::Rectangle<float> area, const juce::String& label,
                    float grDb, float rangeDb, juce::Colour colour);

    ADCompressorProcessor& audioProcessor;
    KnobLookAndFeel lnf;

    juce::Slider knob;
    juce::ToggleButton bypassButton { "Bypass" };

    juce::Label updateLabel;
    juce::TextButton updateButton { "Check for updates" };
    juce::ToggleButton autoCheckToggle { "Check automatically" };
    juce::String pendingReleaseUrl;

    juce::AudioProcessorValueTreeState::SliderAttachment knobAttachment;
    juce::AudioProcessorValueTreeState::ButtonAttachment bypassAttachment;

    float compGr = 0, deEssGr = 0, limitGr = 0, makeup = 0;
    juce::Rectangle<float> meterArea;
    juce::Rectangle<int> footerArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ADCompressorEditor)
};
