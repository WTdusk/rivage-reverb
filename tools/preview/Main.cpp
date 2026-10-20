/*
    Rivage Preview - a small desktop shell around the Rivage Reverb processor.

    It exists so the plugin can be auditioned without a DAW and so the editor can
    be rendered to a PNG for documentation:

        RivagePreview.exe --screenshot shot.png
*/

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"

namespace rivage
{

//==============================================================================
/** Streams the default audio device through the processor and can inject short
    noise bursts so the reverb can be auditioned without an instrument. */
class PreviewAudio final : public juce::AudioIODeviceCallback
{
public:
    explicit PreviewAudio (RivageAudioProcessor& processorToUse) : processor (processorToUse) {}

    ~PreviewAudio() override { stop(); }

    void start()
    {
        deviceManager.initialiseWithDefaultDevices (2, 2);
        deviceManager.addAudioCallback (this);
        running = true;
    }

    void stop()
    {
        if (running)
        {
            deviceManager.removeAudioCallback (this);
            deviceManager.closeAudioDevice();
            running = false;
        }
    }

    void setBurstEnabled (bool shouldEnable) { burstsEnabled = shouldEnable; }

    //==========================================================================
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override
    {
        const int blockSize = juce::jmax (64, device->getCurrentBufferSizeSamples());
        processor.setRateAndBufferSizeDetails (device->getCurrentSampleRate(), blockSize);
        processor.prepareToPlay (device->getCurrentSampleRate(), blockSize);
        scratch.setSize (2, blockSize, false, false, true);
        samplesUntilBurst = 0;
    }

    void audioDeviceStopped() override {}

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        if (numSamples <= 0 || scratch.getNumSamples() < numSamples)
            return;

        juce::AudioBuffer<float> buffer (scratch.getArrayOfWritePointers(), 2, numSamples);
        buffer.clear();

        for (int channel = 0; channel < 2; ++channel)
            if (channel < numInputChannels && inputChannelData[channel] != nullptr)
                buffer.copyFrom (channel, 0, inputChannelData[channel], numSamples);

        if (burstsEnabled)
            addBurst (buffer);

        juce::MidiBuffer midi;
        processor.processBlock (buffer, midi);

        for (int channel = 0; channel < numOutputChannels; ++channel)
            if (outputChannelData[channel] != nullptr)
                juce::FloatVectorOperations::copy (outputChannelData[channel],
                                                   buffer.getReadPointer (juce::jmin (channel, 1)),
                                                   numSamples);
    }

private:
    void addBurst (juce::AudioBuffer<float>& buffer)
    {
        const int numSamples = buffer.getNumSamples();

        for (int i = 0; i < numSamples; ++i)
        {
            if (--samplesUntilBurst <= 0)
            {
                burstRemaining = 480;
                samplesUntilBurst = (int) (bellsPerSecond * 1.0);
            }

            if (burstRemaining > 0)
            {
                --burstRemaining;

                const uint32_t state = (uint32_t) (noiseState = noiseState * 1664525u + 1013904223u);
                const float noise = (float) ((state >> 9) & 0x7FFFFF) / 4194304.0f - 1.0f;
                const float envelope = (float) burstRemaining / 480.0f;
                const float value = noise * envelope * envelope * 0.35f;

                buffer.addSample (0, i, value);
                buffer.addSample (1, i, value * 0.85f);
            }
        }
    }

    juce::AudioDeviceManager deviceManager;
    RivageAudioProcessor& processor;
    juce::AudioBuffer<float> scratch;
    bool running = false;
    bool burstsEnabled = false;
    int samplesUntilBurst = 0;
    int burstRemaining = 0;
    int bellsPerSecond = 24000;
    uint32_t noiseState = 0x1234567u;
};

//==============================================================================
class PreviewWindow final : public juce::DocumentWindow
{
public:
    PreviewWindow (RivageAudioProcessor& processorToUse, bool withBurstToggle)
        : DocumentWindow ("Rivage Reverb", juce::Colour (0xff0a0d10),
                          DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);

        auto* content = new juce::Component();
        auto* editor = processorToUse.createEditor();
        content->addAndMakeVisible (editor);
        editor->setTopLeftPosition (0, 0);

        int height = editor->getHeight();

        if (withBurstToggle)
        {
            burstButton.setButtonText ("TEST BURSTS");
            burstButton.setClickingTogglesState (true);
            burstButton.onClick = [this] { audio->setBurstEnabled (burstButton.getToggleState()); };
            content->addAndMakeVisible (burstButton);
            height += 44;
        }

        content->setSize (editor->getWidth(), height);
        setContentOwned (content, true);
        setResizable (false, false);
        setVisible (true);

        if (withBurstToggle)
        {
            burstButton.setBounds (12, editor->getHeight() + 6, 130, 30);

            audio = std::make_unique<PreviewAudio> (processorToUse);
            audio->start();
        }
    }

    ~PreviewWindow() override
    {
        if (audio != nullptr)
            audio->stop();
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

    juce::TextButton burstButton;
    std::unique_ptr<PreviewAudio> audio;
};

//==============================================================================
class PreviewApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "Rivage Preview"; }
    const juce::String getApplicationVersion() override { return "1.0.0"; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String& commandLine) override
    {
        processor = std::make_unique<RivageAudioProcessor>();

        applySetAssignments (parseSetAssignments (commandLine));

        const auto screenshotPath = parseScreenshotPath (commandLine);

        if (screenshotPath.isNotEmpty())
        {
            window = std::make_unique<PreviewWindow> (*processor, false);
            screenshotTarget = juce::File (screenshotPath);

            juce::Timer::callAfterDelay (600, [this]
            {
                if (auto* editor = window->getContentComponent())
                {
                    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);

                    juce::PNGImageFormat format;
                    if (auto stream = screenshotTarget.createOutputStream())
                        format.writeImageToStream (image, *stream);
                }

                quit();
            });

            return;
        }

        window = std::make_unique<PreviewWindow> (*processor, true);
    }

    void shutdown() override
    {
        window.reset();
        processor.reset();
    }

    void systemRequestedQuit() override { quit(); }

private:
    /** `--set id=value` may be repeated; used to photograph a specific machine
        state for the documentation. */
    static juce::StringArray parseSetAssignments (const juce::String& commandLine)
    {
        juce::StringArray tokens;
        tokens.addTokens (commandLine, true);

        juce::StringArray assignments;

        for (int i = 0; i < tokens.size() - 1; ++i)
            if (tokens[i] == "--set")
                assignments.add (tokens[++i].unquoted());

        return assignments;
    }

    void applySetAssignments (const juce::StringArray& assignments)
    {
        auto& state = processor->getValueTreeState();

        for (const auto& assignment : assignments)
        {
            const auto id = assignment.upToFirstOccurrenceOf ("=", false, false).trim();
            const auto text = assignment.fromFirstOccurrenceOf ("=", false, false).trim();

            if (auto* parameter = state.getParameter (id))
            {
                // Mirror the analyzer: a numeric value is the normalised 0..1
                // position, anything else is the parameter's own text. Without
                // this, "--set drive=0" would be read as a text token and fall
                // back to the default 0.5 (0.0 dB).
                const bool numeric = text.containsOnly ("0123456789.+-eE");

                parameter->setValueNotifyingHost (numeric ? (float) juce::jlimit (0.0, 1.0, text.getDoubleValue())
                                                          : parameter->getValueForText (text));
            }
        }
    }

    static juce::String parseScreenshotPath (const juce::String& commandLine)
    {
        juce::StringArray tokens;
        tokens.addTokens (commandLine, true);

        for (int i = 0; i < tokens.size() - 1; ++i)
            if (tokens[i] == "--screenshot")
                return tokens[++i].unquoted();

        return {};
    }

    std::unique_ptr<RivageAudioProcessor> processor;
    std::unique_ptr<PreviewWindow> window;
    juce::File screenshotTarget;
};

} // namespace rivage

START_JUCE_APPLICATION (rivage::PreviewApplication)
