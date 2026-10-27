#include "Algorithms.h"

namespace rivage
{

namespace
{
// Three parallel diffuser chains per side. No tank, no early reflections: the
// wash exists only because the allpasses keep smearing and the loops keep
// feeding it back.
constexpr float kStageLeftMs[6]  = { 31.7f, 43.1f, 57.7f, 71.3f, 89.9f, 109.1f };
constexpr float kStageRightMs[6] = { 33.1f, 45.7f, 59.3f, 73.9f, 91.7f, 111.7f };
constexpr float kChainGain[6]    = { 0.58f, 0.62f, 0.66f, 0.69f, 0.72f, 0.75f };

constexpr float kFeedbackMs[6] = { 61.3f, 97.7f, 141.1f, 63.7f, 101.3f, 143.9f };
constexpr float kOutputTrim    = 1.3342f;
} // namespace

//==============================================================================
void CloudAlgorithm::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;

    for (int c = 0; c < kChains; ++c)
    {
        for (int s = 0; s < kStages; ++s)
        {
            const float scale = 1.0f + 0.13f * (float) s;

            const int dL = juce::jmax (8, (int) (kStageLeftMs[s] * 0.001 * sampleRate * scale));
            const int dR = juce::jmax (8, (int) (kStageRightMs[s] * 0.001 * sampleRate * scale));
            // Size reaches 1.5x and modulation adds up to 8 ms. Allocate for
            // both, plus cubic interpolation guards, before audio starts.
            const auto capacity = [this, scale] (float milliseconds)
            {
                return (int) std::ceil (sampleRate * (milliseconds * 0.001 * scale * 1.5 + 0.008)) + 8;
            };
            diffusers[(size_t) (c * kStages + s)].prepare (capacity (kStageLeftMs[s]), kChainGain[s], dL);
            diffusers[(size_t) ((kChains + c) * kStages + s)].prepare (capacity (kStageRightMs[s]), kChainGain[s], dR);
        }
    }

    const int maxFeedback = (int) std::ceil (0.30 * sampleRate) + 64;

    for (int c = 0; c < 2 * kChains; ++c)
    {
        feedback[(size_t) c].prepare (maxFeedback);
        feedbackDelay[(size_t) c] = juce::jmax (16.0f, kFeedbackMs[c] * 0.001f * (float) sampleRate);
        feedbackState[(size_t) c] = 0.0f;
        modPhase[(size_t) c] = std::fmod (0.19f * (float) c + 0.23f, 1.0f);
    }

    sine.prepare();

    for (auto* smoother : { &smoothDecay, &smoothSize, &smoothMod, &smoothFreeze })
        smoother->reset (sampleRate, 0.06);

    lowSplit.setCutoff (sampleRate, 250.0);
    midSplit.setCutoff (sampleRate, 3000.0);

    reset();
    updateCoefficients();
}

void CloudAlgorithm::reset()
{
    for (auto& ap : diffusers)
        ap.clear();

    for (int c = 0; c < 2 * kChains; ++c)
    {
        feedback[(size_t) c].clear();
        feedbackState[(size_t) c] = 0.0f;
    }

    counter = 0;
}

void CloudAlgorithm::setParams (const AlgorithmParams& newParams)
{
    params = newParams;

    smoothDecay.setTargetValue (params.decay);
    smoothSize.setTargetValue (params.size);
    smoothMod.setTargetValue (params.mod);
    smoothFreeze.setTargetValue (params.freeze);
}

void CloudAlgorithm::snap()
{
    for (auto* smoother : { &smoothDecay, &smoothSize, &smoothMod, &smoothFreeze })
        smoother->setCurrentAndTargetValue (smoother->getTargetValue());

    updateCoefficients();
}

//==============================================================================
void CloudAlgorithm::updateCoefficients()
{
    // The wash is bounded at the short end by the allpass chains' own ring, so
    // the diffusion gain is tied to decay as well: short decay = less ring.
    const float decayAmount = juce::jlimit (0.0f, 1.0f, smoothDecay.getCurrentValue());
    rt60 = 0.40f * std::pow (3.4f / 0.40f, decayAmount);
    const float chainGain = 0.42f + 0.34f * decayAmount;

    for (int c = 0; c < kChains; ++c)
        for (int s = 0; s < kStages; ++s)
        {
            diffusers[(size_t) (c * kStages + s)].setGain (chainGain);
            diffusers[(size_t) ((kChains + c) * kStages + s)].setGain (chainGain);
        }
    const float sizeScale = 0.7f + 0.8f * smoothSize.getCurrentValue();
    freezeAmount = juce::jlimit (0.0f, 1.0f, smoothFreeze.getCurrentValue());

    const float lowCoeff = (float) (1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 250.0 / sampleRate));
    const float midCoeff = (float) (1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 3000.0 / sampleRate));

    lowSplit.coefficient = lowCoeff;
    midSplit.coefficient = midCoeff;

    const float modRateHz = 0.04f + 0.20f * smoothMod.getCurrentValue();
    const float modSamples = (0.002f + 0.006f * smoothMod.getCurrentValue()) * (float) sampleRate;

    for (int c = 0; c < 2 * kChains; ++c)
    {
        const float delay = juce::jmax (16.0f, kFeedbackMs[c] * 0.001f * (float) sampleRate * sizeScale);
        feedbackDelay[(size_t) c] = delayForFreeze (delay, freezeAmount);
        feedbackGain[(size_t) c] = gainForFreeze (juce::jmin (0.9995f, loopGainForRt60 (delay, sampleRate, rt60)), freezeAmount);

        modInc[(size_t) c] = modRateHz * (0.75f + 0.5f * (float) (c % 4)) / (float) sampleRate;
    }

    // Slow delay modulation on the diffusers is what makes the cloud drift.
    for (int c = 0; c < kChains; ++c)
    {
        float phase = modPhase[(size_t) c] + modInc[(size_t) c] * (float) kChunk;

        if (phase >= 1.0f)
            phase -= 1.0f;

        modPhase[(size_t) c] = phase;

        const float offset = sine.lookup (phase) * modSamples * smoothMod.getCurrentValue() * (1.0f - freezeAmount);

        for (int s = 0; s < kStages; ++s)
        {
            const float baseL = kStageLeftMs[s] * 0.001f * (float) sampleRate * sizeScale * (1.0f + 0.13f * (float) s);
            const float baseR = kStageRightMs[s] * 0.001f * (float) sampleRate * sizeScale * (1.0f + 0.13f * (float) s);

            auto& left = diffusers[(size_t) (c * kStages + s)];
            auto& right = diffusers[(size_t) ((kChains + c) * kStages + s)];
            const float delayL = delayForFreeze (baseL + offset, freezeAmount);
            const float delayR = delayForFreeze (baseR - offset * 0.8f, freezeAmount);
            jassert (delayL <= (float) left.line.buffer.size() - 3.0f);
            jassert (delayR <= (float) right.line.buffer.size() - 3.0f);
            left.setDelay (delayL);
            right.setDelay (delayR);
        }
    }
}

//==============================================================================
void CloudAlgorithm::processSample (float inL, float inR, float& outL, float& outR)
{
    if (++counter >= kChunk)
    {
        counter = 0;
        advanceSmoothers (kChunk, { &smoothDecay, &smoothSize, &smoothMod, &smoothFreeze });
        updateCoefficients();
    }

    const float inputGain = 1.0f - freezeAmount;

    float wetL = 0.0f, wetR = 0.0f;

    for (int c = 0; c < kChains; ++c)
    {
        float chainL = inL * inputGain;
        float chainR = inR * inputGain;

        // Feedback from the previous lap, darkened slightly on the way round.
        const float fbL = feedback[(size_t) c].read (feedbackDelay[(size_t) c]);
        const float fbR = feedback[(size_t) (c + kChains)].read (feedbackDelay[(size_t) (c + kChains)]);

        feedbackState[(size_t) c] += 0.25f * (fbL - feedbackState[(size_t) c]);
        feedbackState[(size_t) (c + kChains)] += 0.25f * (fbR - feedbackState[(size_t) (c + kChains)]);

        // Remove loop damping as Freeze rises, without muting the wet output.
        const float frozenFbL = freezeAmount > 0.0f
            ? feedbackState[(size_t) c] + freezeAmount * (fbL - feedbackState[(size_t) c])
            : feedbackState[(size_t) c];
        const float frozenFbR = freezeAmount > 0.0f
            ? feedbackState[(size_t) (c + kChains)] + freezeAmount * (fbR - feedbackState[(size_t) (c + kChains)])
            : feedbackState[(size_t) (c + kChains)];
        chainL += frozenFbL * feedbackGain[(size_t) c];
        chainR += frozenFbR * feedbackGain[(size_t) (c + kChains)];

        for (int s = 0; s < kStages; ++s)
        {
            chainL = diffusers[(size_t) (c * kStages + s)].process (chainL);
            chainR = diffusers[(size_t) ((kChains + c) * kStages + s)].process (chainR);
        }

        // Cross feed keeps the centre open without making it mono.
        wetL += chainL;
        wetR += chainR;

        // Cross mixing contracts the stereo difference. Fade to independent
        // lossless loops when frozen, preserving both channels' stored energy.
        const float cross = 0.35f * inputGain;
        const float same = 1.0f - cross;
        feedback[(size_t) c].push (chainR * cross + chainL * same);
        feedback[(size_t) (c + kChains)].push (chainL * cross + chainR * same);
    }

    wetL *= 0.42f;
    wetR *= 0.42f;

    // The wash accumulates at long decay settings, so the output is soft
    // saturated instead of hard clipped: transparent at normal levels, never
    // over full scale when the tail piles up.
    outL = std::tanh (wetL * kOutputTrim * 0.9f);
    outR = std::tanh (wetR * kOutputTrim * 0.9f);
}

} // namespace rivage
