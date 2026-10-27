#include "ReverbEngine.h"
#include "Algorithms.h"

namespace rivage
{

//==============================================================================
namespace
{
const char* const kAlgorithmNames[kNumEngines] =
{
    "ROOM", "PLATE", "HALL", "SPRING", "SHIMMER", "CLOUD", "SPRING TANK"
};
} // namespace

const char* algorithmName (int index) noexcept
{
    return kAlgorithmNames[juce::jlimit (0, kNumEngines - 1, index)];
}

/** SPRING's two machines share one slot in the UI but are separate engines. */
static int engineFor (const ReverbParameters& parameters) noexcept
{
    const int algorithm = juce::jlimit (0, kNumAlgorithms - 1, parameters.algorithm);

    if (algorithm == kSpringAlgorithmIndex && parameters.springModel == 1)
        return kSpringTankEngine;

    return algorithm;
}

std::unique_ptr<ReverbAlgorithm> createAlgorithm (int engineIndex)
{
    switch (juce::jlimit (0, kNumEngines - 1, engineIndex))
    {
        case 0:  return std::make_unique<RoomAlgorithm>();
        case 1:  return std::make_unique<PlateAlgorithm>();
        case 3:  return std::make_unique<SpringAlgorithm>();
        case 4:  return std::make_unique<ShimmerAlgorithm>();
        case 5:  return std::make_unique<CloudAlgorithm>();
        case 6:  return std::make_unique<SpringTankAlgorithm>();
        default: return std::make_unique<HallAlgorithm>();
    }
}

//==============================================================================
ReverbEngine::ReverbEngine() = default;
ReverbEngine::~ReverbEngine() = default;

void ReverbEngine::prepare (double newSampleRate, int /*blockSize*/)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;

    const int preDelayMax = (int) std::ceil (0.40 * sampleRate) + 64;
    preDelayL.prepare (preDelayMax);
    preDelayR.prepare (preDelayMax);

    smoothPredelay.reset (sampleRate, 0.05);
    smoothMix.reset (sampleRate, 0.03);
    smoothWidth.reset (sampleRate, 0.03);
    smoothTone.reset (sampleRate, 0.05);
    smoothCurrentGain.reset (sampleRate, 0.04);
    smoothRetiringGain.reset (sampleRate, 0.6);

    lowMonoSide.reset();
    highShelfL.reset();
    highShelfR.reset();
    lowShelfL.reset();
    lowShelfR.reset();
    controlCounter = 0;
    lowMonoSide.setHighPass (sampleRate, (double) kLowMonoHz, 0.707);

    activeEngine = engineFor (parameters);
    current = createAlgorithm (activeEngine);
    current->prepare (sampleRate);

    retiring.reset();

    snapParameters();
}

void ReverbEngine::reset()
{
    preDelayL.clear();
    preDelayR.clear();

    if (current != nullptr)
        current->reset();

    retiring.reset();

    smoothCurrentGain.setCurrentAndTargetValue (1.0f);
    smoothRetiringGain.setCurrentAndTargetValue (0.0f);

    lowMonoSide.reset();
    highShelfL.reset();
    highShelfR.reset();
    lowShelfL.reset();
    lowShelfR.reset();
    controlCounter = 0;

    activeEngine = engineFor (parameters);

    if (current == nullptr)
    {
        current = createAlgorithm (activeEngine);
        current->prepare (sampleRate);
    }

    snapParameters();
}

void ReverbEngine::setParameters (const ReverbParameters& newParameters)
{
    parameters = newParameters;

    const int wanted = engineFor (parameters);

    if (wanted != activeEngine)
        beginSwitch (wanted);

    smoothMix.setTargetValue (juce::jlimit (0.0f, 1.0f, parameters.mix));
    smoothWidth.setTargetValue (juce::jlimit (0.0f, 1.0f, parameters.width));
    smoothTone.setTargetValue (juce::jlimit (0.0f, 1.0f, parameters.tone));
    smoothPredelay.setTargetValue (juce::jlimit (0.0f, 1.0f, parameters.predelay) * 0.30f * (float) sampleRate);

    if (current != nullptr)
    {
        AlgorithmParams p;
        p.decay = juce::jlimit (0.0f, 1.0f, parameters.decay);
        p.size = juce::jlimit (0.0f, 1.0f, parameters.size);
        p.mod = juce::jlimit (0.0f, 1.0f, parameters.mod);
        p.freeze = juce::jlimit (0.0f, 1.0f, parameters.freeze);
        p.drive = juce::jlimit (0.0f, 1.0f, parameters.drive);
        current->setParams (p);
    }
}

void ReverbEngine::snapParameters()
{
    smoothMix.setCurrentAndTargetValue (juce::jlimit (0.0f, 1.0f, parameters.mix));
    smoothWidth.setCurrentAndTargetValue (juce::jlimit (0.0f, 1.0f, parameters.width));
    smoothTone.setCurrentAndTargetValue (juce::jlimit (0.0f, 1.0f, parameters.tone));
    smoothPredelay.setCurrentAndTargetValue (juce::jlimit (0.0f, 1.0f, parameters.predelay) * 0.30f * (float) sampleRate);
    smoothCurrentGain.setCurrentAndTargetValue (1.0f);
    smoothRetiringGain.setCurrentAndTargetValue (0.0f);

    if (current != nullptr)
    {
        AlgorithmParams p;
        p.decay = juce::jlimit (0.0f, 1.0f, parameters.decay);
        p.size = juce::jlimit (0.0f, 1.0f, parameters.size);
        p.mod = juce::jlimit (0.0f, 1.0f, parameters.mod);
        p.freeze = juce::jlimit (0.0f, 1.0f, parameters.freeze);
        p.drive = juce::jlimit (0.0f, 1.0f, parameters.drive);
        current->setParams (p);
        current->snap();
    }

    updateSharedCoefficients();
}

//==============================================================================
void ReverbEngine::beginSwitch (int engineIndex)
{
    if (current == nullptr)
    {
        activeEngine = engineIndex;
        current = createAlgorithm (engineIndex);
        current->prepare (sampleRate);
        current->setParams ({ parameters.decay, parameters.size, parameters.mod, parameters.freeze, parameters.drive });
        current->snap();
        return;
    }

    // A previously retired tail is already faded to nothing; drop it so only one
    // old algorithm is ever alive at a time.
    retiring.reset();

    retiring = std::move (current);
    current = createAlgorithm (engineIndex);
    activeEngine = engineIndex;

    current->prepare (sampleRate);
    current->setParams ({ parameters.decay, parameters.size, parameters.mod, parameters.freeze, parameters.drive });
    current->snap();

    // The new algorithm fades in fast, the old tail fades out slowly, and the
    // old tail keeps running on silence so it decays instead of being cut.
    smoothCurrentGain.setCurrentAndTargetValue (0.0f);
    smoothCurrentGain.setTargetValue (1.0f);
    smoothRetiringGain.setCurrentAndTargetValue (1.0f);
    smoothRetiringGain.setTargetValue (0.0f);
}

void ReverbEngine::releaseRetiringIfSilent()
{
    if (retiring == nullptr)
        return;

    if (! smoothRetiringGain.isSmoothing() && smoothRetiringGain.getCurrentValue() < 0.001f)
        retiring.reset();
}

//==============================================================================
void ReverbEngine::updateSharedCoefficients()
{
    const float tone = smoothTone.getCurrentValue();

    toneHighGainDb = (tone - 0.5f) * 2.0f * 9.0f;
    toneLowGainDb = -(tone - 0.5f) * 2.0f * 5.0f;

    highShelfL.setHighShelf (sampleRate, 3200.0, (double) toneHighGainDb, 0.7);
    highShelfR.setHighShelf (sampleRate, 3200.0, (double) toneHighGainDb, 0.7);
    lowShelfL.setLowShelf (sampleRate, 180.0, (double) toneLowGainDb, 0.7);
    lowShelfR.setLowShelf (sampleRate, 180.0, (double) toneLowGainDb, 0.7);
}

//==============================================================================
void ReverbEngine::process (juce::AudioBuffer<float>& buffer)
{
    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    if (numSamples <= 0 || numChannels <= 0 || current == nullptr)
        return;

    auto* left = buffer.getWritePointer (0);
    auto* right = numChannels > 1 ? buffer.getWritePointer (1) : left;

    for (int i = 0; i < numSamples; ++i)
    {
        if (++controlCounter >= kControlChunk)
        {
            controlCounter = 0;
            if (advanceSmoothers (kControlChunk, { &smoothTone }))
                updateSharedCoefficients();
            releaseRetiringIfSilent();
        }

        const float dryL = left[i];
        const float dryR = right[i];

        preDelayL.push (dryL);
        preDelayR.push (dryR);

        const float delaySamples = juce::jmax (1.0f, smoothPredelay.getNextValue());
        const float inL = preDelayL.read (delaySamples);
        const float inR = preDelayR.read (delaySamples);

        float curL = 0.0f, curR = 0.0f;
        current->processSample (inL, inR, curL, curR);

        const float currentGain = smoothCurrentGain.getNextValue();

        float wetL = curL * currentGain;
        float wetR = curR * currentGain;

        if (retiring != nullptr)
        {
            float oldL = 0.0f, oldR = 0.0f;
            // Keep feeding the old algorithm the same input while it fades, so
            // a sustained note does not lose its tail when the mode changes.
            retiring->processSample (inL, inR, oldL, oldR);

            const float oldGain = smoothRetiringGain.getNextValue();
            wetL += oldL * oldGain;
            wetR += oldR * oldGain;
        }
        else
        {
            smoothRetiringGain.getNextValue();
        }

        // Tone and width belong to the shared back end, so they never jump when
        // the algorithm changes.
        wetL = lowShelfL.process (highShelfL.process (wetL));
        wetR = lowShelfR.process (highShelfR.process (wetR));

        const float mid = 0.5f * (wetL + wetR);
        // Width scales the side signal; the low end of it then goes through a
        // high pass so nothing below kLowMonoHz stays out of phase.
        const float side = 0.5f * (wetL - wetR) * (smoothWidth.getNextValue() * 2.0f);
        const float sideMono = lowMonoSide.process (side);
        wetL = mid + sideMono;
        wetR = mid - sideMono;

        const float mix = juce::jlimit (0.0f, 1.0f, smoothMix.getNextValue());

        if (numChannels == 1)
            left[i] = dryL * (1.0f - mix) + 0.5f * (wetL + wetR) * mix;
        else
        {
            left[i] = dryL * (1.0f - mix) + wetL * mix;
            right[i] = dryR * (1.0f - mix) + wetR * mix;
        }
    }
}

} // namespace rivage
