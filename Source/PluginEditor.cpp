#include "PluginEditor.h"

#include <RivageArt.h>

#include <cmath>

namespace rivage
{

namespace
{
constexpr int kEditorWidth = 860;
constexpr int kEditorHeight = 470;

constexpr float kRotaryStart = juce::MathConstants<float>::pi * 1.25f;
constexpr float kRotaryEnd   = juce::MathConstants<float>::pi * 2.75f;

// rails
static const juce::Rectangle<int> kLeftRail  { 12, 54, 286, 332 };
static const juce::Rectangle<int> kRightRail { 562, 54, 286, 332 };
static const juce::Rectangle<int> kDock      { 12, 394, 836, 38 };

constexpr int kTopBarHeight = 44;
constexpr int kStatusHeight = 32;
} // namespace

//==============================================================================
RivageAudioProcessorEditor::RivageAudioProcessorEditor (RivageAudioProcessor& owner)
    : AudioProcessorEditor (&owner), processor (owner)
{
    setLookAndFeel (&lookAndFeel);

    configureKnob (decayKnob,    paramID::decay,    "DECAY");
    configureKnob (predelayKnob, paramID::predelay, "PRE-DELAY");
    configureKnob (modKnob,      paramID::mod,      "MOD");
    configureKnob (mixKnob,      paramID::mix,      "MIX");
    configureKnob (toneKnob,     paramID::tone,     "TONE");
    configureKnob (sizeKnob,     paramID::size,     "SIZE");
    configureKnob (widthKnob,    paramID::width,    "WIDTH");

    decayKnob.slider.getProperties().set ("hero", true);
    decayKnob.value.setFont (RivageLookAndFeel::monoFont (19.0f));
    decayKnob.name.setFont (RivageLookAndFeel::bodyFont (10.0f));

    // ---- machines (dock) --------------------------------------------------
    static const std::array<const char*, kNumAlgorithms> kTabNames = { "ROOM", "PLATE", "HALL",
                                                                      "SPRING", "SHIMMER", "CLOUD" };

    for (int i = 0; i < kNumAlgorithms; ++i)
    {
        auto& button = algorithmButtons[(size_t) i];
        button.setButtonText (kTabNames[(size_t) i]);
        button.setComponentID ("tab");
        addAndMakeVisible (button);
    }

    if (auto* parameter = processor.getValueTreeState().getParameter (paramID::algorithm))
    {
        algorithmAttachment = std::make_unique<juce::ParameterAttachment> (
            *parameter, [this] (float value)
            {
                refreshAlgorithmButtons ((int) std::lround (value * (float) (kNumAlgorithms - 1)));
            });

        algorithmAttachment->sendInitialUpdate();

        for (int i = 0; i < kNumAlgorithms; ++i)
        {
            algorithmButtons[(size_t) i].onClick = [this, i]()
            {
                algorithmAttachment->setValueAsCompleteGesture ((float) i);
            };
        }
    }

    // ---- freeze -----------------------------------------------------------
    freezeButton.setComponentID ("freeze");
    addAndMakeVisible (freezeButton);

    if (auto* parameter = processor.getValueTreeState().getParameter (paramID::freeze))
        freezeAttachment = std::make_unique<juce::ButtonParameterAttachment> (*parameter, freezeButton);

    setSize (kEditorWidth, kEditorHeight);
    setResizable (false, false);
}

RivageAudioProcessorEditor::~RivageAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

//==============================================================================
void RivageAudioProcessorEditor::configureKnob (Knob& knob, const char* parameterID,
                                                const char* displayName)
{
    auto& slider = knob.slider;
    slider.setRotaryParameters (kRotaryStart, kRotaryEnd, true);
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    slider.setVelocityBasedMode (false);
    slider.setMouseDragSensitivity (180);
    slider.setDoubleClickReturnValue (true, slider.getValue());
    addAndMakeVisible (slider);

    knob.name.setText (displayName, juce::dontSendNotification);
    knob.name.setFont (RivageLookAndFeel::bodyFont (9.5f));
    knob.name.setJustificationType (juce::Justification::centred);
    knob.name.setColour (juce::Label::textColourId, RivageLookAndFeel::muted().withAlpha (0.95f));
    knob.name.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (knob.name);

    knob.value.setFont (RivageLookAndFeel::monoFont (13.5f));
    knob.value.setJustificationType (juce::Justification::centred);
    knob.value.setColour (juce::Label::textColourId, RivageLookAndFeel::ink());
    knob.value.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (knob.value);

    auto& state = processor.getValueTreeState();
    knob.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        state, parameterID, slider);

    if (auto* parameter = state.getParameter (parameterID))
    {
        knob.value.setText (parameter->getCurrentValueAsText(), juce::dontSendNotification);
        knob.slider.onValueChange = [&knob, parameter]()
        {
            knob.value.setText (parameter->getCurrentValueAsText(), juce::dontSendNotification);
        };
    }
}

void RivageAudioProcessorEditor::placeKnob (Knob& knob, juce::Rectangle<int> knobArea,
                                            juce::Rectangle<int> nameArea, juce::Rectangle<int> valueArea)
{
    knob.slider.setBounds (knobArea);
    knob.name.setBounds (nameArea);
    knob.value.setBounds (valueArea);
}

void RivageAudioProcessorEditor::refreshAlgorithmButtons (int activeIndex)
{
    for (int i = 0; i < kNumAlgorithms; ++i)
        algorithmButtons[(size_t) i].setToggleState (i == activeIndex, juce::dontSendNotification);

    repaint();
}

//==============================================================================
void RivageAudioProcessorEditor::drawArtwork (juce::Graphics& g)
{
    if (artwork.isNull())
        artwork = juce::ImageFileFormat::loadFrom (RivageArt::rivagewaterlilies_jpg,
                                                   (size_t) RivageArt::rivagewaterlilies_jpgSize);

    const auto bounds = getLocalBounds().toFloat();

    if (artwork.isValid())
    {
        if (artworkScaled.isNull() || artworkScaledWidth != getWidth()
            || artworkScaledHeight != getHeight())
        {
            const float scale = juce::jmax (bounds.getWidth() / (float) artwork.getWidth(),
                                            bounds.getHeight() / (float) artwork.getHeight());
            artworkScaled = artwork.rescaled (juce::roundToInt ((float) artwork.getWidth() * scale),
                                              juce::roundToInt ((float) artwork.getHeight() * scale),
                                              juce::Graphics::highResamplingQuality);
            artworkScaledWidth = getWidth();
            artworkScaledHeight = getHeight();
        }

        const float x = bounds.getX() + (bounds.getWidth() - (float) artworkScaled.getWidth()) * 0.5f;
        const float y = bounds.getY() + (bounds.getHeight() - (float) artworkScaled.getHeight()) * 0.55f;
        g.drawImage (artworkScaled,
                     juce::Rectangle<float> (x, y, (float) artworkScaled.getWidth(),
                                             (float) artworkScaled.getHeight()),
                     juce::RectanglePlacement::stretchToFit);
    }
    else
    {
        g.fillAll (RivageLookAndFeel::pond());
    }

    g.setColour (RivageLookAndFeel::paper().withAlpha (0.05f));
    g.fillRect (bounds);
}

void RivageAudioProcessorEditor::drawPaperPanel (juce::Graphics& g, juce::Rectangle<float> area,
                                                 float corner, float alpha, bool castShadow)
{
    if (castShadow)
    {
        g.setColour (RivageLookAndFeel::ink().withAlpha (0.045f));
        g.fillRoundedRectangle (area.translated (0.0f, 7.0f).expanded (2.0f), corner + 2.0f);
        g.setColour (RivageLookAndFeel::ink().withAlpha (0.040f));
        g.fillRoundedRectangle (area.translated (0.0f, 3.0f).expanded (0.5f), corner + 0.5f);
    }

    g.setColour (RivageLookAndFeel::paper().withAlpha (alpha));
    g.fillRoundedRectangle (area, corner);

    g.setColour (RivageLookAndFeel::border());
    g.drawRoundedRectangle (area.reduced (0.5f), corner, 1.0f);
}

void RivageAudioProcessorEditor::drawRailHead (juce::Graphics& g, juce::Rectangle<int> area,
                                               const juce::String& index, const juce::String& title)
{
    RivageLookAndFeel::drawTrackedText (g, index, area.removeFromLeft (16).toFloat(),
                                        RivageLookAndFeel::monoFont (8.0f),
                                        RivageLookAndFeel::muted().withAlpha (0.85f), 1.4f,
                                        juce::Justification::centredLeft);

    RivageLookAndFeel::drawTrackedText (g, title, area.toFloat(),
                                        RivageLookAndFeel::displayFont (13.5f),
                                        RivageLookAndFeel::ink(), 0.8f,
                                        juce::Justification::centredLeft);
}

//==============================================================================
void RivageAudioProcessorEditor::paint (juce::Graphics& g)
{
    drawArtwork (g);

    const auto bounds = getLocalBounds().toFloat();

    // ---- top bar ----------------------------------------------------------
    g.setColour (RivageLookAndFeel::paper().withAlpha (0.90f));
    g.fillRect (0.0f, 0.0f, bounds.getWidth(), (float) kTopBarHeight);
    g.setColour (RivageLookAndFeel::hairline());
    g.fillRect (0.0f, (float) kTopBarHeight - 1.0f, bounds.getWidth(), 1.0f);

    {
        auto brandArea = juce::Rectangle<float> (26.0f, 8.0f, 240.0f, 28.0f);
        const auto brandFont = RivageLookAndFeel::displayFont (17.5f);
        const float brandTracking = 3.4f;
        RivageLookAndFeel::drawTrackedText (g, "RIVAGE", brandArea, brandFont,
                                            RivageLookAndFeel::ink(), brandTracking,
                                            juce::Justification::centredLeft);

        const float brandWidth = RivageLookAndFeel::trackedWidth ("RIVAGE", brandFont, brandTracking);
        const float ruleX = brandArea.getX() + brandWidth + 14.0f;

        g.setColour (RivageLookAndFeel::ink().withAlpha (0.30f));
        g.fillRect (ruleX, brandArea.getCentreY() - 10.0f, 1.0f, 20.0f);

        RivageLookAndFeel::drawTrackedText (g, "REVERB",
                                            { ruleX + 12.0f, brandArea.getY(), 120.0f, brandArea.getHeight() },
                                            RivageLookAndFeel::bodyFont (8.5f),
                                            RivageLookAndFeel::muted(), 2.2f,
                                            juce::Justification::centredLeft);

        RivageLookAndFeel::drawTrackedText (g, "GUITAR REVERB  /  SIX MACHINES",
                                            { bounds.getWidth() - 300.0f, brandArea.getY(), 274.0f, brandArea.getHeight() },
                                            RivageLookAndFeel::monoFont (8.0f),
                                            RivageLookAndFeel::muted().withAlpha (0.9f), 1.4f,
                                            juce::Justification::centredRight);
    }

    // ---- rails ------------------------------------------------------------
    drawPaperPanel (g, kLeftRail.toFloat(), 14.0f, 0.985f, true);
    drawPaperPanel (g, kRightRail.toFloat(), 14.0f, 0.985f, true);

    drawRailHead (g, { 30, 68, 96, 18 }, "01", "TIME");
    drawRailHead (g, { 580, 68, 110, 18 }, "02", "SURFACE");

    // ---- dock -------------------------------------------------------------
    drawPaperPanel (g, kDock.toFloat(), 10.0f, 0.97f, true);

    RivageLookAndFeel::drawTrackedText (g, "03", { 26.0f, 402.0f, 14.0f, 20.0f },
                                        RivageLookAndFeel::monoFont (8.0f),
                                        RivageLookAndFeel::muted().withAlpha (0.85f), 1.4f,
                                        juce::Justification::centredLeft);
    RivageLookAndFeel::drawTrackedText (g, "MACHINES", { 42.0f, 402.0f, 90.0f, 20.0f },
                                        RivageLookAndFeel::displayFont (12.5f),
                                        RivageLookAndFeel::ink(), 0.8f,
                                        juce::Justification::centredLeft);

    // ---- status -----------------------------------------------------------
    const float statusY = bounds.getHeight() - (float) kStatusHeight;
    g.setColour (RivageLookAndFeel::paper().withAlpha (0.97f));
    g.fillRect (0.0f, statusY, bounds.getWidth(), (float) kStatusHeight);
    g.setColour (RivageLookAndFeel::hairline());
    g.fillRect (0.0f, statusY, bounds.getWidth(), 1.0f);

    {
        const float textY = statusY + 8.0f;
        g.setColour (RivageLookAndFeel::teal());
        g.fillEllipse (24.0f, textY + 5.0f, 4.0f, 4.0f);

        RivageLookAndFeel::drawTrackedText (g, "WTDUSK", { 34.0f, textY, 90.0f, 14.0f },
                                            RivageLookAndFeel::monoFont (8.0f),
                                            RivageLookAndFeel::muted(), 1.4f,
                                            juce::Justification::centredLeft);

        RivageLookAndFeel::drawTrackedText (g, juce::String::fromUTF8 ("\xe5\xa3\xb0\xe9\x9f\xb3\xe9\x80\x80\xe5\x8e\xbb\xe4\xb9\x8b\xe5\x90\x8e\xef\xbc\x8c\xe7\xa9\xba\xe9\x97\xb4\xe7\x95\x99\xe4\xb8\x8b\xe3\x80\x82"),
                                            { bounds.getCentreX() - 120.0f, textY, 240.0f, 14.0f },
                                            RivageLookAndFeel::bodyFont (9.5f),
                                            RivageLookAndFeel::muted(), 0.4f,
                                            juce::Justification::centred);

        RivageLookAndFeel::drawTrackedText (g, "RIVAGE 0.1",
                                            { bounds.getWidth() - 140.0f, textY, 116.0f, 14.0f },
                                            RivageLookAndFeel::monoFont (8.0f),
                                            RivageLookAndFeel::muted(), 1.4f,
                                            juce::Justification::centredRight);
    }

    // ---- centre stage: the machine that owns the room ---------------------
    {
        int index = kDefaultAlgorithm;

        if (auto* parameter = processor.getValueTreeState().getParameter (paramID::algorithm))
            index = juce::jlimit (0, kNumAlgorithms - 1,
                                  (int) std::lround (parameter->getValue() * (float) (kNumAlgorithms - 1)));

        const auto name = juce::String (algorithmName (index)).toUpperCase();
        const auto indexText = "MACHINE " + juce::String (index + 1).paddedLeft ('0', 2) + " / "
                               + juce::String (kNumAlgorithms).paddedLeft ('0', 2);

        const auto nameFont = RivageLookAndFeel::displayFont (20.0f);
        const auto indexFont = RivageLookAndFeel::monoFont (8.0f);
        const float nameTracking = 2.4f;
        const float indexTracking = 1.4f;

        const float pillWidth = juce::jmax (RivageLookAndFeel::trackedWidth (name, nameFont, nameTracking),
                                            RivageLookAndFeel::trackedWidth (indexText, indexFont, indexTracking))
                                + 58.0f;
        const float pillHeight = 64.0f;
        const auto pill = juce::Rectangle<float> (pillWidth, pillHeight)
                              .withCentre ({ 431.0f, 156.0f });

        drawPaperPanel (g, pill, 14.0f, 0.96f, true);

        RivageLookAndFeel::drawTrackedText (g, name,
                                            { pill.getX(), pill.getY() + 12.0f, pill.getWidth(), 26.0f },
                                            nameFont, RivageLookAndFeel::ink(), nameTracking,
                                            juce::Justification::centred);

        RivageLookAndFeel::drawTrackedText (g, indexText,
                                            { pill.getX(), pill.getY() + 38.0f, pill.getWidth(), 14.0f },
                                            indexFont, RivageLookAndFeel::muted().withAlpha (0.9f),
                                            indexTracking, juce::Justification::centred);
    }
}

//==============================================================================
void RivageAudioProcessorEditor::resized()
{
    // ---- left rail: 01 TIME ----------------------------------------------
    placeKnob (decayKnob, { 99, 88, 112, 112 }, { 74, 204, 160, 13 }, { 74, 218, 160, 18 });
    placeKnob (predelayKnob, { 40, 258, 62, 62 }, { 16, 322, 110, 12 }, { 16, 335, 110, 16 });
    placeKnob (modKnob, { 178, 258, 62, 62 }, { 154, 322, 110, 12 }, { 154, 335, 110, 16 });

    freezeButton.setBounds (32, 352, 246, 22);

    // ---- right rail: 02 SURFACE ------------------------------------------
    placeKnob (mixKnob, { 611, 104, 62, 62 }, { 587, 168, 110, 12 }, { 587, 181, 110, 16 });
    placeKnob (toneKnob, { 736, 104, 62, 62 }, { 712, 168, 110, 12 }, { 712, 181, 110, 16 });
    placeKnob (sizeKnob, { 611, 236, 62, 62 }, { 587, 300, 110, 12 }, { 587, 313, 110, 16 });
    placeKnob (widthKnob, { 736, 236, 62, 62 }, { 712, 300, 110, 12 }, { 712, 313, 110, 16 });

    // ---- dock: 03 MACHINES ------------------------------------------------
    constexpr int tabX = 150;
    constexpr int tabWidth = 104;
    constexpr int tabGap = 10;

    for (int i = 0; i < kNumAlgorithms; ++i)
        algorithmButtons[(size_t) i].setBounds (tabX + i * (tabWidth + tabGap), 398, tabWidth, 30);
}

} // namespace rivage
