#include "Algorithms.h"

namespace rivage
{

namespace
{
// Early reflection pattern: gaps grow with time, like a real box.
constexpr float kTapMs[14] =
{
    2.9f, 4.3f, 6.1f, 7.7f, 9.3f, 11.1f, 13.3f,
    16.1f, 19.3f, 22.7f, 26.3f, 31.1f, 36.7f, 41.3f
};

constexpr float kTapGain[14] =
{
    1.00f, 0.86f, 0.72f, 0.66f, 0.58f, 0.50f, 0.44f,
    0.38f, 0.32f, 0.28f, 0.24f, 0.20f, 0.16f, 0.13f
};

// Low modal bed: the thump of a small box.
constexpr float kModeHz[5]  = { 62.0f, 88.0f, 117.0f, 151.0f, 193.0f };
constexpr float kModeGain[5] = { 0.55f, 0.46f, 0.34f, 0.24f, 0.16f };
constexpr float kOutputTrim  = 0.013452f;

// The tank is short (7.9-19.7 ms), so its low modes are sparse and lightly
// damped: a bass note that lands on one was measured +11.5 dB above unity.
// Two measures tame it: the low band decays a little faster than 1 kHz, and
// the late path is high passed so a low note cannot drive the tank's modal
// region at all. The modal bed is filtered along with it for the same reason.
constexpr float kLowBandRatio = 0.60f;
constexpr float kMidBandRatio = 1.00f;
constexpr float kLateHighPassHz = 80.0f;

constexpr float kTankMs[8] = { 7.9f, 9.7f, 11.3f, 12.7f, 14.9f, 16.3f, 18.1f, 19.7f };

// Linear below the knee so normal playing is untouched, then a tanh curve that
// approaches the ceiling: the old hard jlimit (4.0) was being hit by bass notes.
inline float softClip (float x) noexcept
{
    constexpr float knee = 0.70f;
    constexpr float ceiling = 0.99f;
    const float magnitude = std::abs (x);

    if (magnitude <= knee)
        return x;

    const float shaped = knee + (ceiling - knee) * std::tanh ((magnitude - knee) / (ceiling - knee));
    return x < 0.0f ? -shaped : shaped;
}
} // namespace

//==============================================================================
void RoomAlgorithm::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;

    const int tapBuffer = juce::jmax (16, (int) (0.12 * sampleRate));
    inputBufferL.prepare (tapBuffer);
    inputBufferR.prepare (tapBuffer);

    for (int t = 0; t < kTaps; ++t)
    {
        // Alternating pan, close to centre at the start, wide later on.
        const float pan = (t % 2 == 0 ? -1.0f : 1.0f) * juce::jmin (1.0f, 0.25f + 0.06f * (float) t);
        const float left = std::sqrt (0.5f * (1.0f - pan));
        const float right = std::sqrt (0.5f * (1.0f + pan));

        tapGainL[(size_t) t] = kTapGain[t] * left;
        tapGainR[(size_t) t] = kTapGain[t] * right;
    }

    // The alternating pan plus a descending tap gain biases the loudest taps to
    // one side; equalise both sides so the early reflections stay centred.
    {
        float energyL = 0.0f, energyR = 0.0f;

        for (int t = 0; t < kTaps; ++t)
        {
            energyL += tapGainL[(size_t) t] * tapGainL[(size_t) t];
            energyR += tapGainR[(size_t) t] * tapGainR[(size_t) t];
        }

        const float target = std::sqrt (juce::jmax (1.0e-9f, energyL * energyR));
        const float scaleL = target / std::sqrt (juce::jmax (1.0e-9f, energyL));
        const float scaleR = target / std::sqrt (juce::jmax (1.0e-9f, energyR));

        for (int t = 0; t < kTaps; ++t)
        {
            tapGainL[(size_t) t] *= scaleL;
            tapGainR[(size_t) t] *= scaleR;
        }
    }

    const int maxDelay = (int) std::ceil (0.06 * sampleRate) + 64;

    for (auto& line : lines)
        line.prepare (maxDelay);

    Lcg rng (0x2B7A1D3u);

    for (int i = 0; i < kLines; ++i)
    {
        const auto signFor = [] (int row, int index) noexcept
        {
            int bits = row & index, count = 0;

            while (bits != 0) { ++count; bits &= bits - 1; }

            return (count & 1) != 0 ? -1.0f : 1.0f;
        };

        // Inject with rows 3 and 4: both are orthogonal to the two output rows,
        // so neither channel is favoured by the tank's first pass.
        injectL[(size_t) i] = 0.55f * signFor (3, i);
        injectR[(size_t) i] = 0.55f * signFor (4, i);

        tapOutL[(size_t) i] = 0.35355339f * signFor (1, i);
        tapOutR[(size_t) i] = 0.35355339f * signFor (2, i);

        modPhase[(size_t) i] = std::fmod (0.21f * (float) i + 0.11f, 1.0f);

        (void) rng;
    }

    sine.prepare();

    for (auto* smoother : { &smoothDecay, &smoothSize, &smoothMod, &smoothFreeze })
        smoother->reset (sampleRate, 0.05);

    lowSplit.setCutoff (sampleRate, 400.0);
    midSplit.setCutoff (sampleRate, 2500.0);

    earlyToneL.setCutoff (sampleRate, 7200.0);
    earlyToneR.setCutoff (sampleRate, 7200.0);

    lateHpL.setHighPass (sampleRate, kLateHighPassHz, 0.707);
    lateHpR.setHighPass (sampleRate, kLateHighPassHz, 0.707);

    reset();
    updateCoefficients();
}

void RoomAlgorithm::reset()
{
    inputBufferL.clear();
    inputBufferR.clear();

    for (auto& line : lines)
        line.clear();

    lineLowState.fill (0.0f);
    lineMidState.fill (0.0f);
    modeY1.fill (0.0f);
    modeY2.fill (0.0f);
    earlyToneL.clear();
    earlyToneR.clear();
    lateHpL.reset();
    lateHpR.reset();
    counter = 0;
}

void RoomAlgorithm::setParams (const AlgorithmParams& newParams)
{
    params = newParams;

    smoothDecay.setTargetValue (params.decay);
    smoothSize.setTargetValue (params.size);
    smoothMod.setTargetValue (params.mod);
    smoothFreeze.setTargetValue (params.freeze);
}

void RoomAlgorithm::snap()
{
    for (auto* smoother : { &smoothDecay, &smoothSize, &smoothMod, &smoothFreeze })
        smoother->setCurrentAndTargetValue (smoother->getTargetValue());

    updateCoefficients();
}

//==============================================================================
void RoomAlgorithm::updateCoefficients()
{
    // 0.86 s (tight booth) to 5.3 s (large live room). The low band runs shorter
    // than this and the high band much shorter, so the broadband figure reads a
    // little below the nominal value (about 0.61x at the default position).
    rt60 = 0.86f * std::pow (5.3f / 0.86f, juce::jlimit (0.0f, 1.0f, smoothDecay.getCurrentValue()));
    sizeScale = 0.72f + 0.55f * smoothSize.getCurrentValue();
    freezeAmount = juce::jlimit (0.0f, 1.0f, smoothFreeze.getCurrentValue());

    const float tapScale = sizeScale;

    for (int t = 0; t < kTaps; ++t)
        tapDelay[(size_t) t] = juce::jmax (1.0f, kTapMs[t] * 0.001f * (float) sampleRate * tapScale);

    // Modal bed: fixed low resonances whose Q rises with size.
    for (int m = 0; m < kModes; ++m)
    {
        const double w = 2.0 * juce::MathConstants<double>::pi * kModeHz[m] / sampleRate;
        const double r = 1.0 - 0.012 / (0.4 + (double) sizeScale);

        modeFreq[(size_t) m] = (float) (2.0 * r * std::cos (w));
        modeR[(size_t) m]    = (float) (r * r);
        modeGain[(size_t) m] = kModeGain[m] * (1.0f - (float) r) * 12.0f;
        modePan[(size_t) m]  = (m % 2 == 0) ? -0.35f : 0.35f;
    }

    const float tHigh = rt60 * 0.42f;          // a room eats highs first
    const float lowCoeff = (float) (1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 400.0 / sampleRate));
    const float midCoeff = (float) (1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 2500.0 / sampleRate));

    lowSplit.coefficient = lowCoeff;
    midSplit.coefficient = midCoeff;

    const float modRateHz = 0.05f + 0.25f * smoothMod.getCurrentValue();
    modDepthSamples = (0.0006f + 0.0016f * smoothMod.getCurrentValue()) * (float) sampleRate;
    modDepthSamples *= 1.0f - freezeAmount;

    for (int i = 0; i < kLines; ++i)
    {
        const float delay = juce::jmax (16.0f, kTankMs[i] * 0.001f * (float) sampleRate * sizeScale);
        lineDelay[(size_t) i] = delayForFreeze (delay, freezeAmount);

        const float gLow  = loopGainForRt60 (delay, sampleRate, rt60 * kLowBandRatio);
        const float gMid  = loopGainForRt60 (delay, sampleRate, rt60 * kMidBandRatio);
        const float gHigh = loopGainForRt60 (delay, sampleRate, tHigh);

        lineLowGain[(size_t) i]  = gainForFreeze (juce::jmin (0.9999f, gLow), freezeAmount);
        lineMidGain[(size_t) i]  = gainForFreeze (juce::jmin (0.9999f, gMid), freezeAmount);
        lineHighGain[(size_t) i] = gainForFreeze (juce::jmin (0.9999f, gHigh), freezeAmount);

        modInc[(size_t) i] = modRateHz * (0.8f + 0.4f * (float) (i % 3)) / (float) sampleRate;
    }
}

//==============================================================================
void RoomAlgorithm::processSample (float inL, float inR, float& outL, float& outR)
{
    if (++counter >= kChunk)
    {
        counter = 0;
        if (advanceSmoothers (kChunk, { &smoothDecay, &smoothSize, &smoothMod, &smoothFreeze }))
            updateCoefficients();
    }

    const float inputGain = 1.0f - freezeAmount;
    inputBufferL.push (inL * inputGain);
    inputBufferR.push (inR * inputGain);

    float earlyL = 0.0f, earlyR = 0.0f;

    for (int t = 0; t < kTaps; ++t)
    {
        const float d = tapDelay[(size_t) t];
        earlyL += inputBufferL.read (d) * tapGainL[(size_t) t];
        earlyR += inputBufferR.read (d) * tapGainR[(size_t) t];
    }

    earlyL = earlyToneL.process (earlyL);
    earlyR = earlyToneR.process (earlyR);

    // Modal bed driven by the high passed mono sum: the early reflections keep
    // the body, the late path stays out of the tank's sparse low modes.
    const float lateInL = lateHpL.process (inL);
    const float lateInR = lateHpR.process (inR);
    const float mono = 0.5f * (lateInL + lateInR) * (1.0f - freezeAmount);
    float modes = 0.0f;

    for (int m = 0; m < kModes; ++m)
    {
        const float y = modeGain[(size_t) m] * mono
                      + modeFreq[(size_t) m] * modeY1[(size_t) m]
                      - modeR[(size_t) m] * modeY2[(size_t) m];

        modeY2[(size_t) m] = modeY1[(size_t) m];
        modeY1[(size_t) m] = y;
        modes += y;
    }

    float tap[kLines];

    for (int i = 0; i < kLines; ++i)
    {
        float phase = modPhase[(size_t) i] + modInc[(size_t) i];

        if (phase >= 1.0f)
            phase -= 1.0f;

        modPhase[(size_t) i] = phase;

        const float wobble = sine.lookup (phase) * modDepthSamples * smoothMod.getCurrentValue();
        tap[i] = lines[(size_t) i].read (juce::jmax (8.0f, lineDelay[(size_t) i] + wobble));
    }

    const float bedL = (earlyL * 0.55f + modes * 0.35f) * inputGain;
    const float bedR = (earlyR * 0.55f + modes * 0.35f) * inputGain;

    float tankOutL = 0.0f, tankOutR = 0.0f;

    for (int i = 0; i < kLines; ++i)
    {
        tankOutL += tap[i] * tapOutL[(size_t) i];
        tankOutR += tap[i] * tapOutR[(size_t) i];
    }

    float feedback[kLines];

    for (int i = 0; i < kLines; ++i)
    {
        const float x = tap[i];

        float& low = lineLowState[(size_t) i];
        float& mid = lineMidState[(size_t) i];

        low += lowSplit.coefficient * (x - low);
        mid += midSplit.coefficient * (x - mid);

        const float gL = lineLowGain[(size_t) i], gM = lineMidGain[(size_t) i], gH = lineHighGain[(size_t) i];

        feedback[i] = freezeAmount >= 1.0f ? x
            : low * gL + (mid - low) * gM + (x - mid) * gH;
    }

    hadamard8 (feedback);

    for (int i = 0; i < kLines; ++i)
    {
        lines[(size_t) i].push (feedback[i]
                                + bedL * injectL[(size_t) i]
                                + bedR * injectR[(size_t) i]);
    }

    outL = softClip ((earlyL * inputGain * 0.85f + tankOutL * 0.55f) * kOutputTrim);
    outR = softClip ((earlyR * inputGain * 0.85f + tankOutR * 0.55f) * kOutputTrim);
}

} // namespace rivage
