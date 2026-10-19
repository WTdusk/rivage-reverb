#include "RivageLookAndFeel.h"

#include <cmath>

namespace rivage
{

namespace
{
    juce::Font makeFont (const juce::String& typeface, float height, int style)
    {
        return juce::Font (juce::FontOptions (typeface, height, style));
    }

}

//==============================================================================
RivageLookAndFeel::RivageLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, pond());
    setColour (juce::Slider::textBoxTextColourId, ink());
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, muted());
    setColour (juce::TextButton::textColourOffId, muted());
    setColour (juce::TextButton::textColourOnId, ink());
    setColour (juce::PopupMenu::backgroundColourId, paper());
    setColour (juce::PopupMenu::textColourId, ink());
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accent().withAlpha (0.12f));
}

//==============================================================================
juce::Font RivageLookAndFeel::displayFont (float height)
{
    return makeFont ("Palatino Linotype", height, juce::Font::plain);
}

juce::Font RivageLookAndFeel::bodyFont (float height)
{
    return makeFont ("Segoe UI", height, juce::Font::plain);
}

juce::Font RivageLookAndFeel::monoFont (float height)
{
    return makeFont ("Consolas", height, juce::Font::plain);
}

juce::Font RivageLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return bodyFont (juce::jlimit (10.0f, 12.0f, (float) buttonHeight * 0.34f));
}

float RivageLookAndFeel::trackedWidth (const juce::String& text, const juce::Font& font, float tracking)
{
    juce::GlyphArrangement arranged;
    arranged.addLineOfText (font, text, 0.0f, 0.0f);
    float width = arranged.getBoundingBox (0, -1, true).getWidth();

    if (text.isNotEmpty())
        width += tracking * (float) (text.length() - 1);

    return width;
}

void RivageLookAndFeel::drawTrackedText (juce::Graphics& g, const juce::String& text,
                                         juce::Rectangle<float> area, const juce::Font& font,
                                         juce::Colour colour, float tracking,
                                         juce::Justification justification)
{
    if (text.isEmpty())
        return;

    const float total = trackedWidth (text, font, tracking);
    float x = area.getX();

    if (justification.testFlags (juce::Justification::horizontallyCentred))
        x = area.getX() + (area.getWidth() - total) * 0.5f;
    else if (justification.testFlags (juce::Justification::right))
        x = area.getRight() - total;

    g.setColour (colour);
    g.setFont (font);

    for (int i = 0; i < text.length(); ++i)
    {
        const auto glyph = juce::String::charToString (text[i]);
        const float glyphWidth = trackedWidth (glyph, font, 0.0f);
        g.drawText (glyph, juce::Rectangle<float> (x, area.getY(), glyphWidth + 2.0f, area.getHeight()),
                    juce::Justification (juce::Justification::centredLeft), false);
        x += glyphWidth + tracking;
    }
}

//==============================================================================
void RivageLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                          float sliderPos, float rotaryStartAngle,
                                          float rotaryEndAngle, juce::Slider& slider)
{
    const auto area = juce::Rectangle<int> (x, y, width, height).toFloat();
    const auto centre = area.getCentre();
    const float dial = juce::jmin (area.getWidth(), area.getHeight());
    const bool hero = (bool) slider.getProperties().getWithDefault ("hero", false);
    const bool hover = slider.isMouseOverOrDragging();

    const float arcRadius = dial * 0.392f;
    const float trackThickness = juce::jmax (1.0f, dial * 0.012f);
    const float valueThickness = juce::jmax (2.0f, dial * (hero ? 0.026f : 0.022f));
    const float faceRadius = dial * 0.335f;
    const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    // ---- face -------------------------------------------------------------
    const auto faceArea = juce::Rectangle<float> (faceRadius * 2.0f, faceRadius * 2.0f)
                              .withCentre (centre);

    g.setColour (ink().withAlpha (0.07f));
    g.fillEllipse (faceArea.translated (0.0f, 1.6f).expanded (0.6f));

    juce::ColourGradient faceGradient (juce::Colour (0xfffdfffe),
                                       centre.x - faceRadius * 0.38f, centre.y - faceRadius * 0.46f,
                                       juce::Colour (0xffdde8e5),
                                       centre.x + faceRadius * 0.55f, centre.y + faceRadius * 0.65f,
                                       true);
    faceGradient.addColour (0.58, juce::Colour (0xffeef6f4));
    g.setGradientFill (faceGradient);
    g.fillEllipse (faceArea);

    {
        juce::ColourGradient shade (ink().withAlpha (0.13f), centre.x, centre.y + faceRadius * 0.35f,
                                    ink().withAlpha (0.0f), centre.x, centre.y + faceRadius * 1.05f,
                                    false);
        g.setGradientFill (shade);
        g.fillEllipse (faceArea);
    }

    g.setColour (ink().withAlpha (0.12f));
    g.drawEllipse (faceArea, 1.0f);

    // ---- arcs -------------------------------------------------------------
    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                         rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (ink().withAlpha (0.09f));
    g.strokePath (track, juce::PathStrokeType (trackThickness, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    juce::Path value;
    value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                         rotaryStartAngle, angle, true);
    const auto valueColour = hero ? accent()
                                  : accent().withAlpha (hover ? 0.95f : 0.80f);
    g.setColour (valueColour);
    g.strokePath (value, juce::PathStrokeType (valueThickness, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    // ---- pointer ----------------------------------------------------------
    {
        const float length = faceRadius * (hero ? 0.44f : 0.42f);
        const float startOffset = faceRadius * 0.40f;
        const float pointerWidth = juce::jmax (2.2f, dial * 0.024f);

        juce::Path pointer;
        pointer.addRoundedRectangle (-pointerWidth * 0.5f, -startOffset - length,
                                     pointerWidth, length, pointerWidth * 0.5f);

        g.setColour (hero ? accent() : ink().withAlpha (0.88f));
        g.fillPath (pointer, juce::AffineTransform::rotation (angle)
                                      .translated (centre.x, centre.y));
    }

    g.setColour (ink().withAlpha (0.30f));
    g.fillEllipse (centre.x - 1.6f, centre.y - 1.6f, 3.2f, 3.2f);
}

//==============================================================================
void RivageLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                              const juce::Colour&, bool highlighted, bool down)
{
    const auto id = button.getComponentID();
    const auto bounds = button.getLocalBounds().toFloat();

    if (id == "freeze")
        return;   // the toggle is drawn entirely in drawButtonText

    if (highlighted || down || button.getToggleState())
    {
        g.setColour (ink().withAlpha (down ? 0.09f : (highlighted ? 0.055f : 0.03f)));
        g.fillRoundedRectangle (bounds.reduced (1.0f, 2.0f), 6.0f);
    }
}

void RivageLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button,
                                        bool highlighted, bool down)
{
    const auto bounds = button.getLocalBounds().toFloat();
    const auto id = button.getComponentID();

    if (id == "freeze")
    {
        drawTrackedText (g, "FREEZE", bounds.withTrimmedRight (56.0f).reduced (2.0f, 0.0f),
                         bodyFont (10.5f), ink(), 0.8f, juce::Justification::centredLeft);

        const float pillWidth = 40.0f, pillHeight = 22.0f;
        const auto pill = juce::Rectangle<float> (pillWidth, pillHeight)
                              .withCentre ({ bounds.getRight() - pillWidth * 0.5f - 2.0f,
                                             bounds.getCentreY() });

        g.setColour (button.getToggleState() ? accent() : ink().withAlpha (0.22f));
        g.fillRoundedRectangle (pill, pillHeight * 0.5f);

        const float knobSize = pillHeight - 4.0f;
        const auto knob = juce::Rectangle<float> (knobSize, knobSize)
                              .withCentre ({ button.getToggleState()
                                                 ? pill.getRight() - knobSize * 0.5f - 2.0f
                                                 : pill.getX() + knobSize * 0.5f + 2.0f,
                                             pill.getCentreY() });

        g.setColour (ink().withAlpha (0.18f));
        g.fillEllipse (knob.translated (0.0f, 1.0f));
        g.setColour (paper().brighter (0.5f));
        g.fillEllipse (knob);
        g.setColour (ink().withAlpha (0.10f));
        g.drawEllipse (knob, 0.8f);
        return;
    }

    const bool active = button.getToggleState();
    const auto colour = active ? ink() : (highlighted ? ink().withAlpha (0.85f) : muted());
    const juce::Font font = bodyFont (10.5f);
    const float tracking = 0.7f;

    drawTrackedText (g, button.getButtonText(), bounds, font, colour, tracking,
                     juce::Justification::centred);

    if (active)
    {
        const float textWidth = trackedWidth (button.getButtonText(), font, tracking);
        const float underlineWidth = juce::jmin (textWidth + 6.0f, bounds.getWidth() - 10.0f);
        const auto underline = juce::Rectangle<float> (underlineWidth, 1.6f)
                                   .withCentre ({ bounds.getCentreX(),
                                                  bounds.getBottom() - 6.0f });
        g.setColour (ink());
        g.fillRoundedRectangle (underline, 0.8f);
    }
}

void RivageLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                          bool, bool)
{
    const auto bounds = button.getLocalBounds().toFloat();
    const float size = juce::jmin (18.0f, bounds.getHeight());

    g.setColour (ink().withAlpha (0.10f));
    g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, 1.0f);

    if (button.getToggleState())
    {
        g.setColour (accent());
        g.fillRoundedRectangle (bounds.reduced (3.0f), 2.0f);
    }

    juce::ignoreUnused (size);
}

} // namespace rivage
