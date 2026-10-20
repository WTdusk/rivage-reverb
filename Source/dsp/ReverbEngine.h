#pragma once

#include "DspPrimitives.h"
#include "ReverbAlgorithm.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <memory>

namespace rivage
{

//==============================================================================
/** The five controls a guitar reverb actually needs, plus the algorithm choice.
    All values are 0..1 unless noted; each algorithm maps them to its own
    mechanism. */
struct ReverbParameters
{
    int algorithm = kDefaultAlgorithm;
    float mix = 0.35f;
    float decay = 0.5f;
    float predelay = 0.15f;
    float tone = 0.5f;
    float mod = 0.35f;
    float size = 0.5f;
    float width = 0.5f;
    float freeze = 0.0f;

    /** 0 = SPRING (PURE, the linear tank), 1 = SPRING TANK (the physical one).
        Ignored by every algorithm other than SPRING. */
    int springModel = 0;
    /** Input gain into the spring transducer, 0..1 with 0.5 = unity. TANK only. */
    float drive = 0.5f;
};

//==============================================================================
class ReverbEngine
{
public:
    ReverbEngine();
    ~ReverbEngine();

    void prepare (double sampleRate, int blockSize);
    void reset();

    /** Call every block with the knob positions. Changes are smoothed, and an
        algorithm change is crossfaded against the retiring tail rather than
        cutting it. */
    void setParameters (const ReverbParameters& newParameters);

    /** Jumps every smoother to its target (used right after prepare). */
    void snapParameters();

    void process (juce::AudioBuffer<float>& buffer);

    const ReverbParameters& getParameters() const noexcept { return parameters; }

private:
    static constexpr int kControlChunk = 32;

    void updateSharedCoefficients();
    void beginSwitch (int algorithmIndex);
    void releaseRetiringIfSilent();

    double sampleRate = 48000.0;
    ReverbParameters parameters;
    int controlCounter = 0;

    std::unique_ptr<ReverbAlgorithm> current;
    std::unique_ptr<ReverbAlgorithm> retiring;

    int activeEngine = -1;

    // Shared front end and back end.
    DelayLine preDelayL, preDelayR;
    juce::SmoothedValue<float> smoothPredelay;
    juce::SmoothedValue<float> smoothMix;
    juce::SmoothedValue<float> smoothWidth;
    juce::SmoothedValue<float> smoothTone;
    juce::SmoothedValue<float> smoothCurrentGain;
    juce::SmoothedValue<float> smoothRetiringGain;

    Biquad highShelfL, highShelfR;
    Biquad lowShelfL, lowShelfR;
    float toneHighGainDb = 0.0f;
    float toneLowGainDb = 0.0f;

    // Everything below kLowMonoHz is folded to the centre. A tail whose low end
    // is out of phase between channels reads as wide but hollow, and jumps in
    // position from note to note; keeping the low end mono fixes both.
    static constexpr float kLowMonoHz = 120.0f;
    Biquad lowMonoSide;

    float predelaySamples = 0.0f;
};

} // namespace rivage
