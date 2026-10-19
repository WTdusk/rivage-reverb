#pragma once

#include "ReverbAlgorithm.h"

namespace rivage
{

//==============================================================================
/** A hall: the input enters through a chain of diffusers, but each stage leaks a
    little into the tank, so the tail grows for the first ~80 ms instead of
    appearing at full density. The tank is long and slowly modulated, and the low
    band is allowed to outlive the mid band. */
class HallAlgorithm : public ReverbAlgorithm
{
public:
    void prepare (double newSampleRate) override;
    void reset() override;
    void setParams (const AlgorithmParams& newParams) override;
    void snap() override;
    void processSample (float inL, float inR, float& outL, float& outR) override;
    const char* getName() const override { return "HALL"; }

protected:
    static constexpr int kLines = 16;
    static constexpr int kStages = 4;
    static constexpr int kChunk = 32;

    /** The diffuse core on its own, shared with SHIMMER. */
    void processHallSample (float inL, float inR, float& outL, float& outR);

    void updateCoefficients();

    double sampleRate = 48000.0;
    AlgorithmParams params;
    int counter = 0;

    std::array<Allpass, 2 * kStages> diffusers;
    std::array<float, kStages> stageGain {};

    std::array<DelayLine, kLines> lines;
    std::array<float, kLines> lineDelay {};
    std::array<float, kLines> lineLowGain {}, lineMidGain {}, lineHighGain {};
    std::array<float, kLines> lineLowState {}, lineMidState {};
    std::array<float, kLines> lineInjection {};
    std::array<float, kLines> tapOutL {}, tapOutR {};
    std::array<float, kLines> modPhase {};
    std::array<float, kLines> modInc {};

    OnePole lowSplit, midSplit;
    SineTable sine;

    juce::SmoothedValue<float> smoothDecay, smoothSize, smoothMod, smoothFreeze;
    float rt60 = 3.5f;
    float modDepthSamples = 0.0f;
    float freezeAmount = 0.0f;
    float sizeScale = 1.0f;
};

} // namespace rivage
