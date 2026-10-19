#pragma once

#include "DspPrimitives.h"

#include <memory>

namespace rivage
{

//==============================================================================
/** The five controls every algorithm receives. All are 0..1; the algorithm
    decides what each one means for its own mechanism. */
struct AlgorithmParams
{
    float decay = 0.5f;   // tail length
    float size = 0.5f;    // spatial extent (delay set, spring length, ...)
    float mod = 0.35f;    // modulation amount
    float freeze = 0.0f;  // 0..1, smoothed; 1 holds the tail forever
};

//==============================================================================
/** One reverb machine. Each algorithm owns its own structure; nothing is shared
    between them except the primitives in DspPrimitives.h. */
class ReverbAlgorithm
{
public:
    virtual ~ReverbAlgorithm() = default;

    virtual void prepare (double newSampleRate) = 0;
    virtual void reset() = 0;
    virtual void setParams (const AlgorithmParams& newParams) = 0;

    /** Jumps the internal smoothing straight to the current targets. Called once
        after prepare so the first block already sounds like the knob positions. */
    virtual void snap() = 0;

    /** Consumes one predelayed stereo sample, returns 100% wet stereo. */
    virtual void processSample (float inL, float inR, float& outL, float& outR) = 0;

    void process (const float* inL, const float* inR, float* outL, float* outR, int numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
            processSample (inL[i], inR[i], outL[i], outR[i]);
    }

    virtual const char* getName() const = 0;
};

//==============================================================================
/** Algorithms are numbered in the order they appear in the UI. */
constexpr int kNumAlgorithms = 6;
constexpr int kDefaultAlgorithm = 2; // HALL

const char* algorithmName (int index) noexcept;

std::unique_ptr<ReverbAlgorithm> createAlgorithm (int index);

} // namespace rivage
