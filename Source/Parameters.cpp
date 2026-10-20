#include "Parameters.h"

namespace rivage
{

namespace
{
juce::String percentText (float value, int)
{
    return juce::String (juce::roundToInt (value * 100.0f)) + " %";
}

float percentFromText (const juce::String& text)
{
    return juce::jlimit (0.0f, 1.0f, text.getFloatValue() / 100.0f);
}

juce::String plainText (float value, int)
{
    return juce::String (juce::roundToInt (value * 100.0f));
}

float plainFromText (const juce::String& text)
{
    return juce::jlimit (0.0f, 1.0f, text.getFloatValue() / 100.0f);
}

juce::String predelayText (float value, int)
{
    const float ms = juce::jlimit (0.0f, 1.0f, value) * 300.0f;
    return juce::String (ms, ms < 10.0f ? 1 : 0) + " ms";
}

float predelayFromText (const juce::String& text)
{
    return juce::jlimit (0.0f, 1.0f, text.getFloatValue() / 300.0f);
}

juce::String widthText (float value, int)
{
    return juce::String (juce::roundToInt (juce::jlimit (0.0f, 1.0f, value) * 200.0f)) + " %";
}

float widthFromText (const juce::String& text)
{
    return juce::jlimit (0.0f, 1.0f, text.getFloatValue() / 200.0f);
}

juce::String toneText (float value, int)
{
    return juce::String (juce::roundToInt (juce::jlimit (0.0f, 1.0f, value) * 100.0f));
}

float toneFromText (const juce::String& text)
{
    return juce::jlimit (0.0f, 1.0f, text.getFloatValue() / 100.0f);
}

// Drive is a gain into the spring transducer: 0.5 is unity, the ends are +-18 dB.
constexpr float kDriveRangeDb = 18.0f;

juce::String driveText (float value, int)
{
    const float db = (juce::jlimit (0.0f, 1.0f, value) - 0.5f) * 2.0f * kDriveRangeDb;

    if (std::abs (db) < 0.05f)
        return "0.0 dB";

    return (db > 0.0f ? "+" : "") + juce::String (db, 1) + " dB";
}

float driveFromText (const juce::String& text)
{
    return juce::jlimit (0.0f, 1.0f, 0.5f + text.getFloatValue() / (2.0f * kDriveRangeDb));
}
} // namespace

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    using namespace juce;

    AudioProcessorValueTreeState::ParameterLayout layout;

    StringArray algorithmNames;

    for (int i = 0; i < kNumAlgorithms; ++i)
        algorithmNames.add (algorithmName (i));

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { paramID::algorithm, 1 },
                                                       "Algorithm", algorithmNames, kDefaultAlgorithm));

    auto addFloat = [&layout] (const char* id, const char* name, float defaultValue,
                               std::function<String (float, int)> toText,
                               std::function<float (const String&)> fromText)
    {
        AudioParameterFloatAttributes attributes;

        if (toText)
            attributes = attributes.withStringFromValueFunction (std::move (toText));

        if (fromText)
            attributes = attributes.withValueFromStringFunction (std::move (fromText));

        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name,
                                                           NormalisableRange<float> (0.0f, 1.0f),
                                                           defaultValue, attributes));
    };

    addFloat (paramID::mix,      "Mix",       0.35f, percentText,   percentFromText);
    addFloat (paramID::decay,    "Decay",     0.50f, plainText,     plainFromText);
    addFloat (paramID::predelay, "Pre-Delay", 0.15f, predelayText,  predelayFromText);
    addFloat (paramID::tone,     "Tone",      0.50f, toneText,      toneFromText);
    addFloat (paramID::mod,      "Mod",       0.35f, plainText,     plainFromText);
    addFloat (paramID::size,     "Size",      0.50f, plainText,     plainFromText);
    addFloat (paramID::width,    "Width",     0.50f, widthText,     widthFromText);

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { paramID::freeze, 1 }, "Freeze", false));

    // SPRING has two machines behind one slot: PURE (the old linear tank) is the
    // default so existing sessions sound identical, TANK is opt-in.
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { paramID::springModel, 1 },
                                                       "Spring Model",
                                                       StringArray { "PURE", "TANK" }, 0));

    addFloat (paramID::drive, "Drive", 0.5f, driveText, driveFromText);

    return layout;
}

} // namespace rivage
