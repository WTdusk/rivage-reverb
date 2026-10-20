#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace rivage
{

//==============================================================================
RivageAudioProcessor::RivageAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "RIVAGE", createParameterLayout())
{
    cacheParameterPointers();
    apvts.addParameterListener (paramID::algorithm, this);
}

RivageAudioProcessor::~RivageAudioProcessor()
{
    apvts.removeParameterListener (paramID::algorithm, this);
}

//==============================================================================
void RivageAudioProcessor::cacheParameterPointers()
{
    pAlgorithm = apvts.getRawParameterValue (paramID::algorithm);
    pMix       = apvts.getRawParameterValue (paramID::mix);
    pDecay     = apvts.getRawParameterValue (paramID::decay);
    pPredelay  = apvts.getRawParameterValue (paramID::predelay);
    pTone      = apvts.getRawParameterValue (paramID::tone);
    pMod       = apvts.getRawParameterValue (paramID::mod);
    pSize      = apvts.getRawParameterValue (paramID::size);
    pWidth     = apvts.getRawParameterValue (paramID::width);
    pFreeze    = apvts.getRawParameterValue (paramID::freeze);
    pSpringModel = apvts.getRawParameterValue (paramID::springModel);
    pDrive     = apvts.getRawParameterValue (paramID::drive);
}

juce::String RivageAudioProcessor::getAlgorithmName (int index) const
{
    return algorithmName (index);
}

//==============================================================================
void RivageAudioProcessor::parameterChanged (const juce::String& parameterID, float newValue)
{
    if (parameterID == paramID::algorithm)
    {
        const int index = juce::jlimit (0, kNumAlgorithms - 1, juce::roundToInt (newValue));

        if (onAlgorithmChanged != nullptr)
            onAlgorithmChanged (index);
    }
}

//==============================================================================
void RivageAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock);
    pushTargetsToEngine();
    engine.snapParameters();
}

void RivageAudioProcessor::releaseResources()
{
}

void RivageAudioProcessor::pushTargetsToEngine()
{
    ReverbParameters params;

    params.algorithm = juce::jlimit (0, kNumAlgorithms - 1, juce::roundToInt (pAlgorithm->load()));
    params.mix       = pMix->load();
    params.decay     = pDecay->load();
    params.predelay  = pPredelay->load();
    params.tone      = pTone->load();
    params.mod       = pMod->load();
    params.size      = pSize->load();
    params.width     = pWidth->load();
    params.freeze    = pFreeze->load() > 0.5f ? 1.0f : 0.0f;
    params.springModel = juce::jlimit (0, 1, juce::roundToInt (pSpringModel->load()));
    params.drive     = pDrive->load();

    engine.setParameters (params);
}

//==============================================================================
void RivageAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused (midi);

    const int numChannels = getTotalNumOutputChannels();

    if (numChannels >= 2)
    {
        pushTargetsToEngine();
        engine.process (buffer);
        return;
    }

    if (numChannels == 1 && buffer.getNumSamples() > 0)
    {
        // The engine is stereo; run the mono signal through both sides and fold back.
        monoScratch.setSize (2, buffer.getNumSamples(), false, false, true);
        monoScratch.copyFrom (0, 0, buffer, 0, 0, buffer.getNumSamples());
        monoScratch.copyFrom (1, 0, buffer, 0, 0, buffer.getNumSamples());

        pushTargetsToEngine();
        engine.process (monoScratch);

        buffer.copyFrom (0, 0, monoScratch, 0, 0, buffer.getNumSamples());
    }
}

//==============================================================================
juce::AudioProcessorEditor* RivageAudioProcessor::createEditor()
{
    return new RivageAudioProcessorEditor (*this);
}

//==============================================================================
void RivageAudioProcessor::getStateInformation (juce::MemoryBlock& destination)
{
    if (auto state = apvts.copyState(); state.isValid())
        if (auto xml = state.createXml())
            copyXmlToBinary (*xml, destination);
}

void RivageAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

} // namespace rivage

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new rivage::RivageAudioProcessor();
}
