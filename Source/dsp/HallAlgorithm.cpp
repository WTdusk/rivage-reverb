#include "HallAlgorithm.h"

namespace rivage
{

namespace
{
// Mutually inharmonic tank lengths; the ratios matter more than the values.
constexpr float kLineMs[16] =
{
    41.3f, 47.1f, 53.3f, 59.7f, 67.1f, 71.3f, 79.7f, 83.9f,
    89.9f, 97.1f, 101.3f, 107.9f, 113.1f, 127.7f, 131.9f, 139.3f
};

constexpr float kDiffuseLeftMs[4]  = { 7.3f, 11.9f, 19.1f, 29.7f };
constexpr float kDiffuseRightMs[4] = { 8.1f, 13.1f, 20.7f, 31.3f };
constexpr float kDiffuseGain[4]    = { 0.62f, 0.66f, 0.70f, 0.74f };
constexpr float kStageGain[4]      = { 0.20f, 0.23f, 0.26f, 0.32f };
constexpr float kOutputTrim        = 2.9693f;

inline float lerp (float a, float b, float t) noexcept { return a + (b - a) * t; }
} // namespace

//==============================================================================
void HallAlgorithm::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;

    for (int s = 0; s < kStages; ++s)
    {
        const int dL = juce::jmax (8, (int) (kDiffuseLeftMs[s] * 0.001 * sampleRate));
        const int dR = juce::jmax (8, (int) (kDiffuseRightMs[s] * 0.001 * sampleRate));

        diffusers[(size_t) s].prepare (dL + 8, kDiffuseGain[s], dL);
        diffusers[(size_t) (s + kStages)].prepare (dR + 8, kDiffuseGain[s], dR);

        stageGain[(size_t) s] = kStageGain[s];
    }

    const int maxDelay = (int) std::ceil (0.30 * sampleRate) + 64;

    for (auto& line : lines)
        line.prepare (maxDelay);

    Lcg rng (0x51ED3A7u);

    for (int i = 0; i < kLines; ++i)
    {
        lineInjection[(size_t) i] = 0.28f * rng.signedUnit();

        // Equal magnitude, orthogonal +-1 patterns: Hadamard rows 1 and 2.
        const auto signFor = [] (int row, int index) noexcept
        {
            int bits = row & index, count = 0;

            while (bits != 0) { ++count; bits &= bits - 1; }

            return (count & 1) != 0 ? -1.0f : 1.0f;
        };

        tapOutL[(size_t) i] = 0.25f * signFor (1, i);
        tapOutR[(size_t) i] = 0.25f * signFor (2, i);

        modPhase[(size_t) i] = std::fmod (0.13f * (float) i + 0.37f, 1.0f);
    }

    sine.prepare();

    const double smoothTime = 0.05;

    for (auto* smoother : { &smoothDecay, &smoothSize, &smoothMod, &smoothFreeze })
        smoother->reset (sampleRate, smoothTime);

    lowSplit.setCutoff (sampleRate, 250.0);
    midSplit.setCutoff (sampleRate, 2500.0);

    reset();
    updateCoefficients();
}

void HallAlgorithm::reset()
{
    for (auto& line : lines)
        line.clear();

    for (auto& ap : diffusers)
        ap.clear();

    lineLowState.fill (0.0f);
    lineMidState.fill (0.0f);
    counter = 0;
}

void HallAlgorithm::setParams (const AlgorithmParams& newParams)
{
    params = newParams;

    smoothDecay.setTargetValue (params.decay);
    smoothSize.setTargetValue (params.size);
    smoothMod.setTargetValue (params.mod);
    smoothFreeze.setTargetValue (params.freeze);
}

void HallAlgorithm::snap()
{
    for (auto* smoother : { &smoothDecay, &smoothSize, &smoothMod, &smoothFreeze })
        smoother->setCurrentAndTargetValue (smoother->getTargetValue());

    updateCoefficients();
}

//==============================================================================
void HallAlgorithm::updateCoefficients()
{
    // A hall spans 0.8 s to about 18 s of tail.
    rt60 = 0.8f * std::pow (18.0f / 0.8f, juce::jlimit (0.0f, 1.0f, smoothDecay.getCurrentValue()));
    sizeScale = 0.55f + 0.9f * smoothSize.getCurrentValue();
    freezeAmount = juce::jlimit (0.0f, 1.0f, smoothFreeze.getCurrentValue());

    const float tLow  = rt60 * 1.35f;
    const float tHigh = rt60 * 0.50f;

    const float lowCoeff = (float) (1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 250.0 / sampleRate));
    const float midCoeff = (float) (1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 2500.0 / sampleRate));

    lowSplit.coefficient = lowCoeff;
    midSplit.coefficient = midCoeff;

    const float modRateHz = 0.06f + 0.30f * smoothMod.getCurrentValue();
    const float modSeconds = (0.0015f + 0.0035f * smoothMod.getCurrentValue()) * sizeScale;

    for (int i = 0; i < kLines; ++i)
    {
        const float delay = juce::jmax (16.0f, kLineMs[i] * 0.001f * (float) sampleRate * sizeScale);
        lineDelay[(size_t) i] = delay;

        const float gLow  = loopGainForRt60 (delay, sampleRate, tLow);
        const float gMid  = loopGainForRt60 (delay, sampleRate, rt60);
        const float gHigh = loopGainForRt60 (delay, sampleRate, tHigh);

        lineLowGain[(size_t) i]  = lerp (gLow, 1.0f, freezeAmount);
        lineMidGain[(size_t) i]  = lerp (gMid, 1.0f, freezeAmount);
        lineHighGain[(size_t) i] = lerp (gHigh, 1.0f, freezeAmount);

        modInc[(size_t) i] = modRateHz * (0.7f + 0.5f * (float) (i % 5))
                               / (float) sampleRate;
        modDepthSamples = modSeconds * (float) sampleRate;
    }
}

//==============================================================================
void HallAlgorithm::processSample (float inL, float inR, float& outL, float& outR)
{
    if (++counter >= kChunk)
    {
        counter = 0;
        updateCoefficients();
    }

    processHallSample (inL, inR, outL, outR);

    // The tank taps are deliberately quiet (they are summed incoherently over
    // sixteen lines); this trim brings the wet signal up to a usable level.
    outL = juce::jlimit (-4.0f, 4.0f, outL * kOutputTrim);
    outR = juce::jlimit (-4.0f, 4.0f, outR * kOutputTrim);
}

//==============================================================================
void HallAlgorithm::processHallSample (float inL, float inR, float& outL, float& outR)
{
    const float inputGain = 1.0f - freezeAmount;

    // Progressive diffusion: every stage leaks into the tank, so the density
    // ramps up over the first ~70 ms instead of arriving all at once.
    float dL = inL, dR = inR;
    float sumL = 0.0f, sumR = 0.0f;

    for (int s = 0; s < kStages; ++s)
    {
        dL = diffusers[(size_t) s].process (dL);
        dR = diffusers[(size_t) (s + kStages)].process (dR);

        sumL += dL * stageGain[(size_t) s];
        sumR += dR * stageGain[(size_t) s];
    }

    const float driveL = sumL * inputGain;
    const float driveR = sumR * inputGain;

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

    outL = 0.0f;
    outR = 0.0f;

    for (int i = 0; i < kLines; ++i)
    {
        outL += tap[i] * tapOutL[(size_t) i];
        outR += tap[i] * tapOutR[(size_t) i];
    }

    // Three-band absorption inside the loop.
    float feedback[kLines];

    for (int i = 0; i < kLines; ++i)
    {
        const float x = tap[i];

        float& low = lineLowState[(size_t) i];
        float& mid = lineMidState[(size_t) i];

        low += lowSplit.coefficient * (x - low);
        mid += midSplit.coefficient * (x - mid);

        feedback[i] = low * lineLowGain[(size_t) i]
                    + (mid - low) * lineMidGain[(size_t) i]
                    + (x - mid) * lineHighGain[(size_t) i];
    }

    hadamard16 (feedback);

    for (int i = 0; i < kLines; ++i)
    {
        const float inject = (i & 1) == 0 ? driveL : driveR;
        lines[(size_t) i].push (feedback[i] + inject * lineInjection[(size_t) i]);
    }

    // Keep the tank bounded; the loop gains alone are not enough after a freeze.
    outL *= 1.35f;
    outR *= 1.35f;
}

} // namespace rivage
