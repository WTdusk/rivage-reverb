#include "Algorithms.h"

namespace rivage
{

namespace
{
// Shimmer rides on the hall tank, which is deliberately quiet; this trim
// matches the level of the other algorithms.
constexpr float kOutputTrim = 6.2696f;
} // namespace

//==============================================================================
void ShimmerAlgorithm::prepare (double newSampleRate)
{
    HallAlgorithm::prepare (newSampleRate);

    shifterL.prepare (newSampleRate);
    shifterR.prepare (newSampleRate);
    shifterL.setRatio (2.0f);
    shifterR.setRatio (2.0f);

    smoothShimmer.reset (newSampleRate, 0.15);
    smoothShimmer.setCurrentAndTargetValue (0.40f);

    lastWetL = 0.0f;
    lastWetR = 0.0f;
}

void ShimmerAlgorithm::reset()
{
    HallAlgorithm::reset();

    shifterL.reset();
    shifterR.reset();

    lastWetL = 0.0f;
    lastWetR = 0.0f;
}

void ShimmerAlgorithm::setParams (const AlgorithmParams& newParams)
{
    HallAlgorithm::setParams (newParams);

    // The octave is part of the machine, not a knob: the hall's MOD control
    // only changes how much of it survives, it never turns it off.
    smoothShimmer.setTargetValue (0.40f);
}

void ShimmerAlgorithm::snap()
{
    HallAlgorithm::snap();
    smoothShimmer.setCurrentAndTargetValue (smoothShimmer.getTargetValue());
}

//==============================================================================
void ShimmerAlgorithm::processSample (float inL, float inR, float& outL, float& outR)
{
    if (++counter >= kChunk)
    {
        counter = 0;
        if (advanceSmoothers (kChunk, { &smoothDecay, &smoothSize, &smoothMod, &smoothFreeze }))
            updateCoefficients();
    }

    // The previous wet output, shifted up an octave, is fed back into the tank.
    // Each trip round raises it again, which is where the choir comes from.
    const float amount = smoothShimmer.getCurrentValue() * 0.45f;

    const float fbL = std::tanh (shifterL.process (lastWetL)) * amount;
    const float fbR = std::tanh (shifterR.process (lastWetR)) * amount;

    float wetL = 0.0f, wetR = 0.0f;
    processHallSample (inL + fbL, inR + fbR, wetL, wetR);

    lastWetL = wetL;
    lastWetR = wetR;

    outL = juce::jlimit (-4.0f, 4.0f, wetL * kOutputTrim);
    outR = juce::jlimit (-4.0f, 4.0f, wetR * kOutputTrim);
}

} // namespace rivage
