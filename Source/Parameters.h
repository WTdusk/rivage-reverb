#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "dsp/ReverbEngine.h"

namespace rivage
{

namespace paramID
{
    inline constexpr const char* algorithm = "algorithm";
    inline constexpr const char* mix       = "mix";
    inline constexpr const char* decay     = "decay";
    inline constexpr const char* predelay  = "predelay";
    inline constexpr const char* tone      = "tone";
    inline constexpr const char* mod       = "mod";
    inline constexpr const char* size      = "size";
    inline constexpr const char* width     = "width";
    inline constexpr const char* freeze    = "freeze";
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

} // namespace rivage
