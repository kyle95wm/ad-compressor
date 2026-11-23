#include "PluginEditor.h"

namespace
{
const juce::Colour bg       { 0xff15171c };
const juce::Colour panel    { 0xff1f232b };
const juce::Colour track    { 0xff2d323c };
const juce::Colour accent   { 0xffffb238 };
const juce::Colour textMain { 0xffe8eaf0 };
const juce::Colour textDim  { 0xff8a91a0 };
const juce::Colour good     { 0xff6fd08c };
const juce::Colour warn     { 0xffff5f5f };
} // namespace

// =============================================================================
KnobLookAndFeel::KnobLookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, textMain);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
}

void KnobLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                        float startAngle, float endAngle, juce::Slider&)
{
    const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (10.0f);
    const float radius = std::min (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle = startAngle + pos * (endAngle - startAngle);
    const float arcW = radius * 0.09f;
    const float arcR = radius - arcW * 0.5f;

    juce::Path bgArc;
    bgArc.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
    g.setColour (track);
    g.strokePath (bgArc, juce::PathStrokeType (arcW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (pos > 0.0f)
    {
        juce::Path valueArc;
        valueArc.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, startAngle, angle, true);
        g.setColour (accent);
        g.strokePath (valueArc, juce::PathStrokeType (arcW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Knob body.
    const float bodyR = radius * 0.72f;
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3a404c), centre.x, centre.y - bodyR,
                                             juce::Colour (0xff22262e), centre.x, centre.y + bodyR, false));
    g.fillEllipse (centre.x - bodyR, centre.y - bodyR, bodyR * 2, bodyR * 2);
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.drawEllipse (centre.x - bodyR, centre.y - bodyR, bodyR * 2, bodyR * 2, 1.5f);

    // Pointer.
    juce::Path pointer;
    const float pw = radius * 0.06f;
    pointer.addRoundedRectangle (-pw * 0.5f, -bodyR * 0.92f, pw, bodyR * 0.38f, pw * 0.5f);
    pointer.applyTransform (juce::AffineTransform::rotation (angle).translated (centre));
    g.setColour (textMain);
    g.fillPath (pointer);
}

void KnobLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool)
{
    const auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    const bool on = b.getToggleState();
    const auto& props = b.getProperties();
    const auto onColour = props.contains ("onColour") ? juce::Colour ((juce::uint32) (juce::int64) props["onColour"]) : accent;
    const auto onText  = props.getWithDefault ("onText", "BYPASSED").toString();
    const auto offText = props.getWithDefault ("offText", "ACTIVE").toString();

    g.setColour (on ? onColour : (highlighted ? track.brighter (0.15f) : track));
    g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
    g.setColour (on ? bg : textMain);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawText (on ? onText : offText, r, juce::Justification::centred);
}

// =============================================================================
ADCompressorEditor::ADCompressorEditor (ADCompressorProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p),
      knobAttachment (p.apvts, ADCompressorProcessor::amountId, knob),
      bypassAttachment (p.apvts, ADCompressorProcessor::bypassId, bypassButton)
{
    knob.setLookAndFeel (&lnf);
    knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    knob.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                              juce::MathConstants<float>::pi * 2.75f, true);
    knob.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 24);
    knob.setDoubleClickReturnValue (true, 50.0);
    knob.setVelocityBasedMode (false);
    addAndMakeVisible (knob);

    bypassButton.setLookAndFeel (&lnf);
    bypassButton.setTooltip ("Level-matched A/B: output loudness tracks the input, so bypassing compares tone, not volume.");
    addAndMakeVisible (bypassButton);

    // Ceiling switch: -1 dBFS (off) / -6 dBFS (on), drawn as the same pill as Bypass.
    ceilingButton.setLookAndFeel (&lnf);
    ceilingButton.getProperties().set ("offText", "CEILING -1 dBFS");
    ceilingButton.getProperties().set ("onText", "CEILING -6 dBFS");
    ceilingButton.getProperties().set ("onColour", (juce::int64) 0xff5ec8f2);
    ceilingButton.setTooltip ("Limiter ceiling. -1 dBFS works at any recording level. -6 dBFS is for takes "
                              "recorded with headroom; on hot takes it will audibly limit consonants.");
    addAndMakeVisible (ceilingButton);
    ceilingAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        p.apvts, ADCompressorProcessor::ceilingId, ceilingButton);

    // --- Footer: version + update checker (same behaviour as VODucker) ------
    updateLabel.setFont (juce::FontOptions (12.0f));
    updateLabel.setColour (juce::Label::textColourId, textDim);
    updateLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (updateLabel);

    updateButton.setColour (juce::TextButton::buttonColourId, track);
    updateButton.setColour (juce::TextButton::textColourOffId, textMain);
    updateButton.onClick = [this]
    {
        if (pendingReleaseUrl.isNotEmpty())
            juce::URL (pendingReleaseUrl).launchInDefaultBrowser();
        else
            audioProcessor.updateChecker.checkNow (true);
        refreshUpdateUi();
    };
    addAndMakeVisible (updateButton);

    autoCheckToggle.setColour (juce::ToggleButton::textColourId, textDim);
    autoCheckToggle.setColour (juce::ToggleButton::tickColourId, accent);
    autoCheckToggle.setColour (juce::ToggleButton::tickDisabledColourId, textDim);
    autoCheckToggle.setToggleState (audioProcessor.updateChecker.isAutoCheckEnabled(), juce::dontSendNotification);
    autoCheckToggle.onClick = [this] { audioProcessor.updateChecker.setAutoCheckEnabled (autoCheckToggle.getToggleState()); };
    addAndMakeVisible (autoCheckToggle);

    audioProcessor.updateChecker.checkNow (false); // throttled to once a day, respects the toggle

    setSize (340, 540);
    startTimerHz (30);
    refreshUpdateUi();
}

ADCompressorEditor::~ADCompressorEditor()
{
    knob.setLookAndFeel (nullptr);
    bypassButton.setLookAndFeel (nullptr);
    ceilingButton.setLookAndFeel (nullptr);
}

void ADCompressorEditor::timerCallback()
{
    const auto& m = audioProcessor.getMeters();
    // Fast attack, slow fall so the meters are readable.
    auto ballistics = [] (float& v, float target) { v = target > v ? target : v * 0.85f + target * 0.15f; };
    ballistics (compGr, m.compGrDb.load());
    ballistics (deEssGr, m.deEssGrDb.load());
    ballistics (limitGr, m.limitGrDb.load());
    makeup = makeup * 0.8f + m.makeupDb.load() * 0.2f;
    repaint (meterArea.toNearestInt().expanded (2));
    refreshUpdateUi();
}

void ADCompressorEditor::refreshUpdateUi()
{
    using S = UpdateChecker::Status;
    const auto r = audioProcessor.updateChecker.getResult();

    pendingReleaseUrl.clear();
    juce::String text, button = "Check for updates";
    auto colour = textDim;

    switch (r.status)
    {
        case S::idle:      break;
        case S::checking:  text = "Checking GitHub..."; break;
        case S::upToDate:  text = "Up to date"; colour = good; break;
        case S::failed:    text = r.error; colour = warn; break;
        case S::updateAvailable:
            text = "Update available: v" + r.latestVersion;
            colour = accent;
            button = "Download";
            pendingReleaseUrl = r.releaseUrl;
            break;
    }

    if (updateLabel.getText() != text)
        updateLabel.setText (text, juce::dontSendNotification);
    updateLabel.setColour (juce::Label::textColourId, colour);
    if (updateButton.getButtonText() != button)
        updateButton.setButtonText (button);
}

void ADCompressorEditor::paint (juce::Graphics& g)
{
    g.fillAll (bg);

    auto area = getLocalBounds().toFloat().reduced (16.0f);

    g.setColour (textMain);
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    auto titleRow = area.removeFromTop (28.0f);
    g.drawText ("ADCompressor", titleRow, juce::Justification::centredLeft);
    g.setColour (textDim);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("v" + UpdateChecker::getCurrentVersion(), titleRow, juce::Justification::centredRight);
    g.setColour (textDim);
    g.setFont (juce::FontOptions (12.0f));
    // Non-ASCII text must be decoded as UTF-8 explicitly (a plain literal is read as Latin-1).
    g.drawText (juce::String (juce::CharPointer_UTF8 ("Snappy VO  \xc2\xb7  3-stage comp  \xc2\xb7  de-ess  \xc2\xb7  limit")),
                area.removeFromTop (18.0f),
                juce::Justification::centredLeft);

    // Footer divider.
    g.setColour (track);
    g.fillRect (footerArea.getX(), footerArea.getY(), footerArea.getWidth(), 1);

    // Meter panel.
    g.setColour (panel);
    g.fillRoundedRectangle (meterArea.expanded (8.0f), 8.0f);

    auto rows = meterArea;
    const float rowH = rows.getHeight() / 4.0f;
    drawMeter (g, rows.removeFromTop (rowH), "COMP",   compGr,  24.0f, accent);
    drawMeter (g, rows.removeFromTop (rowH), "DE-ESS", deEssGr, 12.0f, juce::Colour (0xff5ec8f2));
    drawMeter (g, rows.removeFromTop (rowH), "LIMIT",  limitGr, 12.0f, juce::Colour (0xffff5f5f));

    auto matchRow = rows;
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.setColour (textDim);
    g.drawText ("MATCH", matchRow.removeFromLeft (56.0f), juce::Justification::centredLeft);
    g.setColour (textMain);
    g.drawText ((makeup >= 0 ? "+" : "") + juce::String (makeup, 1) + " dB makeup  (auto)",
                matchRow, juce::Justification::centredLeft);
}

void ADCompressorEditor::drawMeter (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& label,
                                    float grDb, float rangeDb, juce::Colour colour)
{
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
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

void ADCompressorEditor::resized()
{
    auto area = getLocalBounds().reduced (16);
    area.removeFromTop (46);

    // Footer (update checker), separated from the main controls.
    footerArea = area.removeFromBottom (58);
    {
        auto f = footerArea.withTrimmedTop (10);
        auto row1 = f.removeFromTop (22);
        updateLabel.setBounds (row1);
        f.removeFromTop (4);
        auto row2 = f.removeFromTop (24);
        updateButton.setBounds (row2.removeFromRight (140));
        autoCheckToggle.setBounds (row2.withTrimmedRight (8));
    }
    area.removeFromBottom (10);

    auto bottom = area.removeFromBottom (36);
    {
        auto row = bottom.withSizeKeepingCentre (296, 30);
        bypassButton.setBounds (row.removeFromLeft (130));
        ceilingButton.setBounds (row.removeFromRight (150));
    }
    area.removeFromBottom (14);

    meterArea = area.removeFromBottom (104).reduced (8).toFloat();
    area.removeFromBottom (12);

    knob.setBounds (area.withSizeKeepingCentre (std::min (area.getWidth(), area.getHeight()),
                                                area.getHeight()));
}
