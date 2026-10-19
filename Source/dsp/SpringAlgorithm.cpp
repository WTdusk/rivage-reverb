#include "Algorithms.h"

namespace rivage
{

namespace
{
// Six coils of different length, like a real spring tank.
constexpr float kCoilMs[6] = { 21.7f, 26.3f, 31.1f, 35.9f, 41.3f, 47.1f };

// The dispersion coefficient of a spring rises with frequency; a 24 stage
// cascade of first order allpasses turns a click into a descending chirp.
constexpr float kDispersionGain = 0.62f;
constexpr float kOutputTrim     = 0.44896f;
} // namespace

//==============================================================================
void SpringAlgorithm::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;

    const int maxDelay = (int) std::ceil (0.08 * sampleRate) + 64;

    for (auto& coil : coils)
        coil.prepare (maxDelay);

    for (int i = 0; i < kSprings; ++i)
    {
        injectGain[(size_t) i] = (i % 2 == 0 ? 0.55f : -0.55f) * (0.8f + 0.2f * (float) i);
        // Both pickups hear the same coils, so they are strongly correlated;
        // the small lateral term only adds a hint of stereo width.
        const float common  = (i % 3 == 0 ? 1.0f : (i % 3 == 1 ? -0.75f : 0.55f));
        const float lateral = (i & 1) == 0 ? 0.22f : -0.22f;

        pickupL[(size_t) i] = 0.50f * (common + lateral);
        pickupR[(size_t) i] = 0.50f * (common - lateral);

        for (auto& state : dispersionState[(size_t) i])
            state = 0.0f;
    }

    for (auto* smoother : { &smoothDecay, &smoothSize, &smoothFreeze })
        smoother->reset (sampleRate, 0.05);

    // Springs are thin at the bottom and honky in the upper mid.
    inputHighPassL.setCutoff (sampleRate, 280.0);
    inputHighPassR.setCutoff (sampleRate, 280.0);

    toneL.setLowPass (sampleRate, 5200.0, 0.7);
    toneR.setLowPass (sampleRate, 5400.0, 0.7);

    coilPeakL.setPeak (sampleRate, 1900.0, 1.1, 5.5);
    coilPeakR.setPeak (sampleRate, 2300.0, 1.1, 5.0);

    reset();
    updateCoefficients();
}

void SpringAlgorithm::reset()
{
    for (auto& coil : coils)
        coil.clear();

    for (auto& states : dispersionState)
        for (auto& state : states)
            state = 0.0f;

    inputHighPassL.clear();
    inputHighPassR.clear();
    toneL.reset();
    toneR.reset();
    coilPeakL.reset();
    coilPeakR.reset();
    counter = 0;
}

void SpringAlgorithm::setParams (const AlgorithmParams& newParams)
{
    params = newParams;

    smoothDecay.setTargetValue (params.decay);
    smoothSize.setTargetValue (params.size);
    smoothFreeze.setTargetValue (params.freeze);
}

void SpringAlgorithm::snap()
{
    for (auto* smoother : { &smoothDecay, &smoothSize, &smoothFreeze })
        smoother->setCurrentAndTargetValue (smoother->getTargetValue());

    updateCoefficients();
}

//==============================================================================
void SpringAlgorithm::updateCoefficients()
{
    // 0.9 s (short tank) to 6 s (a long, loose tank with the feedback up).
    rt60 = 0.9f * std::pow (6.0f / 0.9f, juce::jlimit (0.0f, 1.0f, smoothDecay.getCurrentValue()));
    const float sizeScale = 0.78f + 0.50f * smoothSize.getCurrentValue();
    freezeAmount = juce::jlimit (0.0f, 1.0f, smoothFreeze.getCurrentValue());

    dispersionCoefficient = kDispersionGain;

    for (int i = 0; i < kSprings; ++i)
    {
        const float delay = juce::jmax (32.0f, kCoilMs[i] * 0.001f * (float) sampleRate * sizeScale);
        coilDelay[(size_t) i] = delay;

        // Springs lose their top end quickly; the low mid rings longest.
        const float g = loopGainForRt60 (delay, sampleRate, rt60 * 1.05f);
        coilGain[(size_t) i] = juce::jmin (0.9999f, g);
    }
}

//==============================================================================
void SpringAlgorithm::processSample (float inL, float inR, float& outL, float& outR)
{
    if (++counter >= kChunk)
    {
        counter = 0;
        updateCoefficients();
    }

    const float inputGain = 1.0f - freezeAmount;

    const float mono = 0.5f * (inL + inR);
    const float excite = inputHighPassL.process (mono) * inputGain;

    float spring[kSprings];

    for (int i = 0; i < kSprings; ++i)
    {
        float y = coils[(size_t) i].read (coilDelay[(size_t) i]);

        // Dispersion cascade inside the loop: this is the spring sound.
        auto& states = dispersionState[(size_t) i];

        for (int s = 0; s < kDispersion; ++s)
        {
            const float delayed = states[(size_t) s];
            const float v = y + dispersionCoefficient * delayed;
            states[(size_t) s] = v;
            y = delayed - dispersionCoefficient * v;
        }

        spring[i] = y;

        const float feedback = y * coilGain[(size_t) i];
        coils[(size_t) i].push (feedback + excite * injectGain[(size_t) i]);

        // A little of the dispersed signal is picked up acoustically too.
        spring[i] += excite * injectGain[(size_t) i] * 0.15f;
    }

    float wetL = 0.0f, wetR = 0.0f;

    for (int i = 0; i < kSprings; ++i)
    {
        wetL += spring[i] * pickupL[(size_t) i];
        wetR += spring[i] * pickupR[(size_t) i];
    }

    wetL = coilPeakL.process (toneL.process (wetL * 2.0f));
    wetR = coilPeakR.process (toneR.process (wetR * 2.0f));

    outL = juce::jlimit (-4.0f, 4.0f, wetL * kOutputTrim);
    outR = juce::jlimit (-4.0f, 4.0f, wetR * kOutputTrim);
}

} // namespace rivage
