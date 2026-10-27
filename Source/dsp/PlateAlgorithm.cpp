#include "Algorithms.h"

namespace rivage
{

namespace
{
// A plate is a metal sheet: dense, bright, and it smears the transient before
// any tail exists. Five allpass stages per side, deliberately prime-related.
constexpr float kDiffuseLeftMs[5]  = { 1.31f, 2.27f, 3.61f, 5.47f, 8.09f };
constexpr float kDiffuseRightMs[5] = { 1.57f, 2.51f, 3.87f, 5.83f, 8.53f };
constexpr float kDiffuseGain[5]    = { 0.62f, 0.66f, 0.70f, 0.73f, 0.76f };
constexpr float kOutputTrim        = 0.22761f;

// The tank is short: plate decay comes from the loop gain, not from length.
constexpr float kTankMs[8] = { 3.1f, 4.3f, 5.9f, 7.3f, 8.9f, 10.7f, 12.7f, 14.3f };
} // namespace

//==============================================================================
void PlateAlgorithm::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;

    for (int s = 0; s < kDiffusers; ++s)
    {
        const int dL = juce::jmax (8, (int) (kDiffuseLeftMs[s] * 0.001 * sampleRate));
        const int dR = juce::jmax (8, (int) (kDiffuseRightMs[s] * 0.001 * sampleRate));

        diffusers[(size_t) s].prepare (dL + 8, kDiffuseGain[s], dL);
        diffusers[(size_t) (s + kDiffusers)].prepare (dR + 8, kDiffuseGain[s], dR);
    }

    const int maxDelay = (int) std::ceil (0.05 * sampleRate) + 64;

    for (auto& line : lines)
        line.prepare (maxDelay);

    for (int i = 0; i < kLines; ++i)
    {
        lineInjection[(size_t) i] = (i & 1) == 0 ? 0.35355339f : -0.35355339f;

        const auto signFor = [] (int row, int index) noexcept
        {
            int bits = row & index, count = 0;

            while (bits != 0) { ++count; bits &= bits - 1; }

            return (count & 1) != 0 ? -1.0f : 1.0f;
        };

        tapOutL[(size_t) i] = 0.35355339f * signFor (1, i);
        tapOutR[(size_t) i] = 0.35355339f * signFor (2, i);

        modPhase[(size_t) i] = std::fmod (0.17f * (float) i + 0.43f, 1.0f);
    }

    sine.prepare();

    for (auto* smoother : { &smoothDecay, &smoothSize, &smoothMod, &smoothFreeze })
        smoother->reset (sampleRate, 0.05);

    lowSplit.setCutoff (sampleRate, 500.0);
    midSplit.setCutoff (sampleRate, 3000.0);

    // The plate's fixed sheen: born bright, not a control.
    sheenL.setHighShelf (sampleRate, 3800.0, 4.0, 0.7);
    sheenR.setHighShelf (sampleRate, 3800.0, 4.0, 0.7);

    reset();
    updateCoefficients();
}

void PlateAlgorithm::reset()
{
    for (auto& ap : diffusers)
        ap.clear();

    for (auto& line : lines)
        line.clear();

    lineLowState.fill (0.0f);
    lineMidState.fill (0.0f);
    sheenL.reset();
    sheenR.reset();
    counter = 0;
}

void PlateAlgorithm::setParams (const AlgorithmParams& newParams)
{
    params = newParams;

    smoothDecay.setTargetValue (params.decay);
    smoothSize.setTargetValue (params.size);
    smoothMod.setTargetValue (params.mod);
    smoothFreeze.setTargetValue (params.freeze);
}

void PlateAlgorithm::snap()
{
    for (auto* smoother : { &smoothDecay, &smoothSize, &smoothMod, &smoothFreeze })
        smoother->setCurrentAndTargetValue (smoother->getTargetValue());

    updateCoefficients();
}

//==============================================================================
void PlateAlgorithm::updateCoefficients()
{
    // 0.8 s to 9 s; a plate is long, but never as long as a hall.
    rt60 = 0.8f * std::pow (9.0f / 0.8f, juce::jlimit (0.0f, 1.0f, smoothDecay.getCurrentValue()));
    const float sizeScale = 0.80f + 0.55f * smoothSize.getCurrentValue();
    freezeAmount = juce::jlimit (0.0f, 1.0f, smoothFreeze.getCurrentValue());

    // Thin at the bottom, extended at the top: that is what makes it a plate.
    const float tLow  = rt60 * 0.72f;
    const float tHigh = rt60 * 1.00f;

    const float lowCoeff = (float) (1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 500.0 / sampleRate));
    const float midCoeff = (float) (1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 3000.0 / sampleRate));

    lowSplit.coefficient = lowCoeff;
    midSplit.coefficient = midCoeff;

    const float modRateHz = 0.08f + 0.35f * smoothMod.getCurrentValue();
    modDepthSamples = 0.0004f * (float) sampleRate * smoothMod.getCurrentValue();
    modDepthSamples *= 1.0f - freezeAmount;

    for (int i = 0; i < kLines; ++i)
    {
        const float delay = juce::jmax (16.0f, kTankMs[i] * 0.001f * (float) sampleRate * sizeScale);
        lineDelay[(size_t) i] = delayForFreeze (delay, freezeAmount);

        const float gLow  = loopGainForRt60 (delay, sampleRate, tLow);
        const float gMid  = loopGainForRt60 (delay, sampleRate, rt60);
        const float gHigh = loopGainForRt60 (delay, sampleRate, tHigh);

        lineLowGain[(size_t) i]  = gainForFreeze (juce::jmin (0.9999f, gLow), freezeAmount);
        lineMidGain[(size_t) i]  = gainForFreeze (juce::jmin (0.9999f, gMid), freezeAmount);
        lineHighGain[(size_t) i] = gainForFreeze (juce::jmin (0.9999f, gHigh), freezeAmount);

        modInc[(size_t) i] = modRateHz * (0.7f + 0.5f * (float) (i % 4)) / (float) sampleRate;
    }
}

//==============================================================================
void PlateAlgorithm::processSample (float inL, float inR, float& outL, float& outR)
{
    if (++counter >= kChunk)
    {
        counter = 0;
        if (advanceSmoothers (kChunk, { &smoothDecay, &smoothSize, &smoothMod, &smoothFreeze }))
            updateCoefficients();
    }

    const float inputGain = 1.0f - freezeAmount;

    float dL = inL, dR = inR;

    for (int s = 0; s < kDiffusers; ++s)
    {
        dL = diffusers[(size_t) s].process (dL);
        dR = diffusers[(size_t) (s + kDiffusers)].process (dR);
    }

    const float driveL = dL * inputGain;
    const float driveR = dR * inputGain;

    float tap[kLines];

    for (int i = 0; i < kLines; ++i)
    {
        float phase = modPhase[(size_t) i] + modInc[(size_t) i];

        if (phase >= 1.0f)
            phase -= 1.0f;

        modPhase[(size_t) i] = phase;

        const float wobble = sine.lookup (phase) * modDepthSamples;
        tap[i] = lines[(size_t) i].read (juce::jmax (8.0f, lineDelay[(size_t) i] + wobble));
    }

    float wetL = 0.0f, wetR = 0.0f;

    for (int i = 0; i < kLines; ++i)
    {
        wetL += tap[i] * tapOutL[(size_t) i];
        wetR += tap[i] * tapOutR[(size_t) i];
    }

    float feedback[kLines];

    for (int i = 0; i < kLines; ++i)
    {
        const float x = tap[i];

        float& low = lineLowState[(size_t) i];
        float& mid = lineMidState[(size_t) i];

        low += lowSplit.coefficient * (x - low);
        mid += midSplit.coefficient * (x - mid);

        feedback[i] = freezeAmount >= 1.0f ? x : low * lineLowGain[(size_t) i]
                    + (mid - low) * lineMidGain[(size_t) i]
                    + (x - mid) * lineHighGain[(size_t) i];
    }

    hadamard8 (feedback);

    for (int i = 0; i < kLines; ++i)
    {
        const float inject = (i & 1) == 0 ? driveL : driveR;
        lines[(size_t) i].push (feedback[i] + inject * lineInjection[(size_t) i]);
    }

    outL = juce::jlimit (-4.0f, 4.0f, sheenL.process (wetL * 1.6f) * kOutputTrim);
    outR = juce::jlimit (-4.0f, 4.0f, sheenR.process (wetR * 1.6f) * kOutputTrim);
}

} // namespace rivage
