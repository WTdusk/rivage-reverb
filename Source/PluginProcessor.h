#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Parameters.h"
#include "dsp/ReverbEngine.h"

namespace rivage
{

class RivageAudioProcessor final : public juce::AudioProcessor,
                                 private juce::AudioProcessorValueTreeState::Listener
{
public:
    RivageAudioProcessor();
    ~RivageAudioProcessor() override;

    //==========================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Rivage Reverb"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 60.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //==========================================================================
    juce::AudioProcessorValueTreeState& getValueTreeState() noexcept { return apvts; }

    /** Called when the algorithm changes so the editor can refresh its layout. */
    std::function<void (int)> onAlgorithmChanged;

    juce::String getAlgorithmName (int index) const;

private:
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void cacheParameterPointers();
    void pushTargetsToEngine();

    juce::AudioProcessorValueTreeState apvts;

    std::atomic<float>* pAlgorithm = nullptr;
    std::atomic<float>* pMix       = nullptr;
    std::atomic<float>* pDecay     = nullptr;
    std::atomic<float>* pPredelay  = nullptr;
    std::atomic<float>* pTone      = nullptr;
    std::atomic<float>* pMod       = nullptr;
    std::atomic<float>* pSize      = nullptr;
    std::atomic<float>* pWidth     = nullptr;
    std::atomic<float>* pFreeze    = nullptr;

    ReverbEngine engine;
    juce::AudioBuffer<float> monoScratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RivageAudioProcessor)
};

} // namespace rivage
