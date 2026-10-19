#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace rivage
{

/**
    Visual language:

    - a water-lily painting fills the window,
    - translucent paper rails sit on top of it (matte, never glass),
    - one deep-water teal is the only accent,
    - controls are drawn like instruments: pale domed faces, hairline arcs.

    Colour tokens below are OKLCH values converted to sRGB.
*/
class RivageLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    RivageLookAndFeel();

    //==================================================================== palette
    static juce::Colour paper()    { return juce::Colour (0xffeef9f7); }   // --surface
    static juce::Colour ink()      { return juce::Colour (0xff0a3942); }   // --fg
    static juce::Colour muted()    { return juce::Colour (0xff405b5e); }   // --muted
    static juce::Colour accent()   { return juce::Colour (0xff006864); }   // deep water teal
    static juce::Colour teal()     { return juce::Colour (0xff3d9791); }   // --paint-teal
    static juce::Colour pond()     { return juce::Colour (0xffd3eeee); }   // --bg
    static juce::Colour border()   { return ink().withAlpha (0.18f); }
    static juce::Colour hairline() { return ink().withAlpha (0.12f); }

    //==================================================================== type
    static juce::Font displayFont (float height);
    static juce::Font bodyFont (float height);
    static juce::Font monoFont (float height);

    /** Draws text with explicit letter spacing (the handoff tracks its small caps). */
    static void drawTrackedText (juce::Graphics&, const juce::String& text,
                                 juce::Rectangle<float> area, const juce::Font&,
                                 juce::Colour, float tracking, juce::Justification);

    static float trackedWidth (const juce::String& text, const juce::Font&, float tracking);

    //==================================================================== painting
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&,
                               bool highlighted, bool down) override;

    void drawButtonText (juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RivageLookAndFeel)
};

} // namespace rivage
