#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "ui/RivageLookAndFeel.h"

namespace rivage
{

/** Six machines, seven knobs, one switch. The painted surface carries the rest. */
class RivageAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit RivageAudioProcessorEditor (RivageAudioProcessor&);
    ~RivageAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Knob
    {
        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
        juce::Label name;
        juce::Label value;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    void configureKnob (Knob&, const char* parameterID, const char* displayName);
    void placeKnob (Knob&, juce::Rectangle<int> knobArea, juce::Rectangle<int> nameArea,
                    juce::Rectangle<int> valueArea);
    void refreshAlgorithmButtons (int activeIndex);

    void drawArtwork (juce::Graphics&);
    void drawPaperPanel (juce::Graphics&, juce::Rectangle<float> area, float corner,
                         float alpha, bool castShadow);
    void drawRailHead (juce::Graphics&, juce::Rectangle<int> area, const juce::String& index,
                       const juce::String& title);

    RivageAudioProcessor& processor;
    RivageLookAndFeel lookAndFeel;
    juce::Image artwork, artworkScaled;
    int artworkScaledWidth = 0, artworkScaledHeight = 0;

    Knob mixKnob, decayKnob, predelayKnob, toneKnob, modKnob, sizeKnob, widthKnob;

    std::array<juce::TextButton, kNumAlgorithms> algorithmButtons;
    std::unique_ptr<juce::ParameterAttachment> algorithmAttachment;

    juce::TextButton freezeButton;
    std::unique_ptr<juce::ButtonParameterAttachment> freezeAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RivageAudioProcessorEditor)
};

} // namespace rivage
