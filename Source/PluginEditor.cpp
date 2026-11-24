#include "PluginEditor.h"

using namespace brand;

// =============================================================================
ADCompressorEditor::ADCompressorEditor (ADCompressorProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p),
      footer (p.updateChecker),
      knobAttachment (p.apvts, ADCompressorProcessor::amountId, knob),
      bypassAttachment (p.apvts, ADCompressorProcessor::bypassId, bypassButton)
{
    setLookAndFeel (&lnf);

    // Add controls before styling them, so the slider's text box picks up the
    // brand look-and-feel (see the template's editor).
    addAndMakeVisible (knob);
    knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    knob.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                              juce::MathConstants<float>::pi * 2.75f, true);
    knob.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 24);
    knob.setDoubleClickReturnValue (true, 50.0);
    knob.setVelocityBasedMode (false);

    makePill (bypassButton, "ACTIVE", "BYPASSED");
    bypassButton.setTooltip ("Level-matched A/B: output loudness tracks the input, so bypassing compares tone, not volume.");
    addAndMakeVisible (bypassButton);

    // Ceiling switch: -1 dBFS (off) / -6 dBFS (on), drawn as the same pill as Bypass.
    makePill (ceilingButton, "CEILING -1 dBFS", "CEILING -6 dBFS", juce::Colour (0xff5ec8f2));
    ceilingButton.setTooltip ("Limiter ceiling. -1 dBFS works at any recording level. -6 dBFS is for takes "
                              "recorded with headroom; on hot takes it will audibly limit consonants.");
    addAndMakeVisible (ceilingButton);
    ceilingAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        p.apvts, ADCompressorProcessor::ceilingId, ceilingButton);

    addAndMakeVisible (footer);

    setSize (340, 566);
    startTimerHz (30);
}

ADCompressorEditor::~ADCompressorEditor()
{
    setLookAndFeel (nullptr);
}

void ADCompressorEditor::timerCallback()
{
    const auto& m = audioProcessor.getMeters();
    // Fast attack, slow fall so the meters are readable.
    auto ballistics = [] (float& v, float target) { v = target > v ? target : v * 0.85f + target * 0.15f; };
    ride = ride * 0.7f + m.rideDb.load() * 0.3f; // bipolar, so smooth both ways
    ballistics (compGr, m.compGrDb.load());
    ballistics (deEssGr, m.deEssGrDb.load());
    ballistics (limitGr, m.limitGrDb.load());
    makeup = makeup * 0.8f + m.makeupDb.load() * 0.2f;
    repaint (meterArea.toNearestInt().expanded (2));
}

void ADCompressorEditor::paint (juce::Graphics& g)
{
    g.fillAll (bg);
    // Non-ASCII text must go through CharPointer_UTF8 (see BrandLookAndFeel.h).
    drawHeader (g, getLocalBounds(), JucePlugin_Name, UpdateChecker::getCurrentVersion(),
                juce::String (juce::CharPointer_UTF8 ("Snappy VO  \xc2\xb7  ride  \xc2\xb7  3-stage comp  \xc2\xb7  de-ess  \xc2\xb7  limit")));

    // Meter panel.
    g.setColour (panel);
    g.fillRoundedRectangle (meterArea.expanded (8.0f), 8.0f);

    auto rows = meterArea;
    const float rowH = rows.getHeight() / 5.0f;
    drawRideMeter (g, rows.removeFromTop (rowH), ride, 6.0f);
    drawMeter (g, rows.removeFromTop (rowH), "COMP",   compGr,  24.0f, accent);
    drawMeter (g, rows.removeFromTop (rowH), "DE-ESS", deEssGr, 12.0f, juce::Colour (0xff5ec8f2));
    drawMeter (g, rows.removeFromTop (rowH), "LIMIT",  limitGr, 12.0f, warn);

    auto matchRow = rows;
    g.setFont (juce::FontOptions (captionSize, juce::Font::bold));
    g.setColour (textDim);
    g.drawText ("MATCH", matchRow.removeFromLeft (56.0f), juce::Justification::centredLeft);
    g.setColour (textMain);
    g.drawText ((makeup >= 0 ? "+" : "") + juce::String (makeup, 1) + " dB makeup  (auto)",
                matchRow, juce::Justification::centredLeft);
}

void ADCompressorEditor::drawMeter (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& label,
                                    float grDb, float rangeDb, juce::Colour colour)
{
    g.setFont (juce::FontOptions (captionSize, juce::Font::bold));
    g.setColour (textDim);
    g.drawText (label, area.removeFromLeft (56.0f), juce::Justification::centredLeft);

    auto valueArea = area.removeFromRight (54.0f);
    g.setColour (textMain);
    g.drawText (grDb >= 0.05f ? "-" + juce::String (grDb, 1) : juce::String ("0.0"), valueArea,
                juce::Justification::centredRight);

    auto bar = area.reduced (4.0f, area.getHeight() * 0.3f);
    g.setColour (track);
    g.fillRoundedRectangle (bar, 3.0f);
    const float frac = juce::jlimit (0.0f, 1.0f, grDb / rangeDb);
    if (frac > 0.0f)
    {
        g.setColour (colour);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * frac), 3.0f);
    }
}

// Rider gain is bipolar (lift or cut), so its bar grows from the centre.
void ADCompressorEditor::drawRideMeter (juce::Graphics& g, juce::Rectangle<float> area, float rideDb, float rangeDb)
{
    g.setFont (juce::FontOptions (captionSize, juce::Font::bold));
    g.setColour (textDim);
    g.drawText ("RIDE", area.removeFromLeft (56.0f), juce::Justification::centredLeft);

    auto valueArea = area.removeFromRight (54.0f);
    g.setColour (textMain);
    const bool zero = std::abs (rideDb) < 0.05f;
    g.drawText (zero ? juce::String ("0.0") : (rideDb > 0 ? "+" : "") + juce::String (rideDb, 1), valueArea,
                juce::Justification::centredRight);

    auto bar = area.reduced (4.0f, area.getHeight() * 0.3f);
    g.setColour (track);
    g.fillRoundedRectangle (bar, 3.0f);
    const float half = bar.getWidth() * 0.5f;
    const float w = half * juce::jlimit (0.0f, 1.0f, std::abs (rideDb) / rangeDb);
    if (w > 0.5f)
    {
        g.setColour (juce::Colour (0xffb48cf2));
        g.fillRoundedRectangle (rideDb > 0 ? bar.withX (bar.getCentreX()).withWidth (w)
                                           : bar.withX (bar.getCentreX() - w).withWidth (w), 3.0f);
    }
    g.setColour (textDim);
    g.fillRect (bar.getCentreX() - 0.5f, bar.getY() - 2.0f, 1.0f, bar.getHeight() + 4.0f);
}

void ADCompressorEditor::resized()
{
    auto area = getLocalBounds().reduced (margin);
    area.removeFromTop (headerHeight);

    footer.setBounds (area.removeFromBottom (footerHeight));
    area.removeFromBottom (10);

    auto bottom = area.removeFromBottom (36);
    {
        auto row = bottom.withSizeKeepingCentre (296, 30);
        bypassButton.setBounds (row.removeFromLeft (130));
        ceilingButton.setBounds (row.removeFromRight (150));
    }
    area.removeFromBottom (14);

    meterArea = area.removeFromBottom (130).reduced (8).toFloat();
    area.removeFromBottom (12);

    knob.setBounds (area.withSizeKeepingCentre (std::min (area.getWidth(), area.getHeight()),
                                                area.getHeight()));
}
