#pragma once

#include "ReverbAlgorithm.h"
#include "HallAlgorithm.h"

namespace rivage
{

//==============================================================================
/** A room: a cluster of coloured early reflections plus a low modal bed, feeding
    a deliberately small tank. The character lives in the first 60 ms, not in the
    tail. */
class RoomAlgorithm : public ReverbAlgorithm
{
public:
    void prepare (double newSampleRate) override;
    void reset() override;
    void setParams (const AlgorithmParams& newParams) override;
    void snap() override;
    void processSample (float inL, float inR, float& outL, float& outR) override;
    const char* getName() const override { return "ROOM"; }

private:
    static constexpr int kTaps = 14;
    static constexpr int kLines = 8;
    static constexpr int kModes = 5;
    static constexpr int kChunk = 32;

    void updateCoefficients();

    double sampleRate = 48000.0;
    AlgorithmParams params;
    int counter = 0;

    DelayLine inputBufferL, inputBufferR;
    std::array<float, kTaps> tapSamplesL {}, tapSamplesR {};
    std::array<float, kTaps> tapDelay {};
    std::array<float, kTaps> tapGainL {}, tapGainR {};

    std::array<float, kModes> modeFreq {};
    std::array<float, kModes> modeR {};
    std::array<float, kModes> modeGain {};
    std::array<float, kModes> modeY1 {}, modeY2 {};
    std::array<float, kModes> modePan {};

    std::array<DelayLine, kLines> lines;
    std::array<float, kLines> lineDelay {};
    std::array<float, kLines> lineLowGain {}, lineMidGain {}, lineHighGain {};
    std::array<float, kLines> lineLowState {}, lineMidState {};
    std::array<float, kLines> injectL {}, injectR {};
    std::array<float, kLines> tapOutL {}, tapOutR {};
    std::array<float, kLines> modPhase {};
    std::array<float, kLines> modInc {};

    OnePole lowSplit, midSplit;
    OnePole earlyToneL, earlyToneR;
    Biquad lateHpL, lateHpR;
    SineTable sine;

    juce::SmoothedValue<float> smoothDecay, smoothSize, smoothMod, smoothFreeze;
    float rt60 = 1.2f;
    float sizeScale = 1.0f;
    float modDepthSamples = 0.0f;
    float freezeAmount = 0.0f;
};

//==============================================================================
/** A plate: no early reflections at all, an eight stage allpass wall in front of
    a short, bright tank. The "sheen" is a fixed high shelf that the plate is born
    with, not a user control. */
class PlateAlgorithm : public ReverbAlgorithm
{
public:
    void prepare (double newSampleRate) override;
    void reset() override;
    void setParams (const AlgorithmParams& newParams) override;
    void snap() override;
    void processSample (float inL, float inR, float& outL, float& outR) override;
    const char* getName() const override { return "PLATE"; }

private:
    static constexpr int kDiffusers = 5;   // per channel
    static constexpr int kLines = 8;
    static constexpr int kChunk = 32;

    void updateCoefficients();

    double sampleRate = 48000.0;
    AlgorithmParams params;
    int counter = 0;

    std::array<Allpass, 2 * kDiffusers> diffusers;

    std::array<DelayLine, kLines> lines;
    std::array<float, kLines> lineDelay {};
    std::array<float, kLines> lineLowGain {}, lineMidGain {}, lineHighGain {};
    std::array<float, kLines> lineLowState {}, lineMidState {};
    std::array<float, kLines> lineInjection {};
    std::array<float, kLines> tapOutL {}, tapOutR {};
    std::array<float, kLines> modPhase {};
    std::array<float, kLines> modInc {};

    OnePole lowSplit, midSplit;
    Biquad sheenL, sheenR;

    SineTable sine;

    juce::SmoothedValue<float> smoothDecay, smoothSize, smoothMod, smoothFreeze;
    float rt60 = 3.0f;
    float modDepthSamples = 0.0f;
    float freezeAmount = 0.0f;
};

//==============================================================================
/** A spring tank: six coiled delay loops, each with a cascade of first order
    allpasses inside the loop. The dispersion is the whole point: it stretches a
    transient into a descending chirp, which is what a spring sounds like. */
class SpringAlgorithm : public ReverbAlgorithm
{
public:
    void prepare (double newSampleRate) override;
    void reset() override;
    void setParams (const AlgorithmParams& newParams) override;
    void snap() override;
    void processSample (float inL, float inR, float& outL, float& outR) override;
    const char* getName() const override { return "SPRING"; }

private:
    static constexpr int kSprings = 6;
    static constexpr int kDispersion = 24;
    static constexpr int kChunk = 32;

    void updateCoefficients();

    double sampleRate = 48000.0;
    AlgorithmParams params;
    int counter = 0;

    std::array<DelayLine, kSprings> coils;
    std::array<std::array<float, kDispersion>, kSprings> dispersionState {};
    std::array<float, kSprings> coilDelay {};
    std::array<float, kSprings> coilGain {};
    std::array<float, kSprings> injectGain {};
    std::array<float, kSprings> pickupL {}, pickupR {};
    std::array<float, kSprings> driveL {}, driveR {};

    OnePole inputHighPassL, inputHighPassR;
    Biquad toneL, toneR;
    Biquad coilPeakL, coilPeakR;

    juce::SmoothedValue<float> smoothDecay, smoothSize, smoothFreeze;
    float rt60 = 2.2f;
    float freezeAmount = 0.0f;
    float dispersionCoefficient = 0.62f;
};

//==============================================================================
/** A cloud: no tank and no reflections, just a long chain of allpasses per
    channel with cross feeds. There is no geometry to hear, which is exactly the
    point. */
class CloudAlgorithm : public ReverbAlgorithm
{
public:
    void prepare (double newSampleRate) override;
    void reset() override;
    void setParams (const AlgorithmParams& newParams) override;
    void snap() override;
    void processSample (float inL, float inR, float& outL, float& outR) override;
    const char* getName() const override { return "CLOUD"; }

private:
    static constexpr int kChains = 3;
    static constexpr int kStages = 6;
    static constexpr int kChunk = 32;

    void updateCoefficients();

    double sampleRate = 48000.0;
    AlgorithmParams params;
    int counter = 0;

    std::array<Allpass, 2 * kChains * kStages> diffusers;
    std::array<DelayLine, 2 * kChains> feedback;
    std::array<float, 2 * kChains> feedbackDelay {};
    std::array<float, 2 * kChains> feedbackGain {};
    std::array<float, 2 * kChains> feedbackState {};

    OnePole lowSplit, midSplit;

    SineTable sine;
    std::array<float, 2 * kChains> modPhase {};
    std::array<float, 2 * kChains> modInc {};

    juce::SmoothedValue<float> smoothDecay, smoothSize, smoothMod, smoothFreeze;
    float rt60 = 5.0f;
    float freezeAmount = 0.0f;
};

//==============================================================================
/** The hall's diffuse core with a pitch shifter hanging off the feedback path,
    so every pass through the tank returns an octave higher. */
class ShimmerAlgorithm : public HallAlgorithm
{
public:
    void prepare (double newSampleRate) override;
    void reset() override;
    void setParams (const AlgorithmParams& newParams) override;
    void snap() override;
    void processSample (float inL, float inR, float& outL, float& outR) override;
    const char* getName() const override { return "SHIMMER"; }

private:
    PitchShifter shifterL, shifterR;
    juce::SmoothedValue<float> smoothShimmer;
    float lastWetL = 0.0f, lastWetR = 0.0f;
};

} // namespace rivage
