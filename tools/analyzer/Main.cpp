/*
    RvxAnalyzer - headless VST3 measurement tool.

    Commands:
      scan   <plugin.vst3>
      render <plugin.vst3> --out <file.wav> [options]

    The tool loads a VST3 with JUCE's headless hosting module, prints its full
    parameter model as JSON, and can render deterministic test signals through
    it so the resulting impulse responses can be analysed offline.
*/

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "dsp/ReverbEngine.h"

#include <chrono>
#include <cstdio>
#include <vector>
#include <cmath>

#include "DspChecks.h"
#include "FreezeChecks.h"

#if JUCE_DEBUG && JUCE_MSVC
 #include <crtdbg.h>
#endif

#if JUCE_WINDOWS
 #include <windows.h>
 #include <shellapi.h>
#endif

using namespace juce;

//==============================================================================
namespace
{

// Support plug-in paths containing non-ASCII characters: the command line
// has to be read as UTF-16 and converted to UTF-8 rather than going through the
// ANSI code page that the C runtime would use for argv.
StringArray getCommandLineArguments (int argc, char** argv)
{
    StringArray result;

   #if JUCE_WINDOWS
    int wideCount = 0;
    if (auto** wide = ::CommandLineToArgvW (::GetCommandLineW(), &wideCount))
    {
        for (int i = 0; i < wideCount; ++i)
            result.add (String (wide[i]));

        ::LocalFree (wide);
        return result;
    }
   #endif

    for (int i = 0; i < argc; ++i)
        result.add (String::fromUTF8 (argv[i]));

    return result;
}

} // namespace


//==============================================================================
namespace
{

AudioPluginFormatManager& formatManager()
{
    static AudioPluginFormatManager& manager = *[]
    {
        auto* m = new AudioPluginFormatManager();
        addHeadlessDefaultFormatsToManager (*m);
        return m;
    }();

    return manager;
}

//==============================================================================
String jsonEscape (const String& s)
{
    String out;
    for (auto c : s)
    {
        if (c == '"' || c == '\\')      { out += '\\'; out += c; }
        else if (c == '\n')             { out += "\\n"; }
        else if (c == '\r')             { out += "\\r"; }
        else if (c == '\t')             { out += "\\t"; }
        else if ((int) c < 32)          { out += ' '; }
        else                            { out += c; }
    }
    return out;
}

String jsonString (const String& s)  { return "\"" + jsonEscape (s) + "\""; }
String jsonNumber (double v, int digits = 6) { return String (v, digits); }

//==============================================================================
bool writeFloatWav (const File& file, const AudioBuffer<float>& buffer, double sampleRate)
{
    const auto numChannels = buffer.getNumChannels();
    const auto numSamples = buffer.getNumSamples();

    std::vector<uint8_t> bytes;
    bytes.reserve ((size_t) (44 + numSamples * numChannels * 4));

    auto putU32 = [&bytes] (uint32_t v) { for (int i = 0; i < 4; ++i) bytes.push_back ((uint8_t) (v >> (8 * i))); };
    auto putU16 = [&bytes] (uint16_t v) { bytes.push_back ((uint8_t) v); bytes.push_back ((uint8_t) (v >> 8)); };
    auto putTag = [&bytes] (const char* t) { for (int i = 0; i < 4; ++i) bytes.push_back ((uint8_t) t[i]); };

    const auto dataSize = (uint32_t) (numSamples * numChannels * 4);

    putTag ("RIFF");
    putU32 (36u + dataSize);
    putTag ("WAVE");
    putTag ("fmt ");
    putU32 (16);
    putU16 (3);                                  // IEEE float
    putU16 ((uint16_t) numChannels);
    putU32 ((uint32_t) sampleRate);
    putU32 ((uint32_t) sampleRate * 4u * (uint32_t) numChannels);
    putU16 ((uint16_t) (4 * numChannels));
    putU16 (32);
    putTag ("data");
    putU32 (dataSize);

    for (int i = 0; i < numSamples; ++i)
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float v = buffer.getReadPointer (ch)[i];
            uint32_t bits;
            std::memcpy (&bits, &v, 4);
            putU32 (bits);
        }

    if (auto stream = file.createOutputStream())
    {
        stream->write (bytes.data(), bytes.size());
        return true;
    }

    return false;
}

//==============================================================================
struct PluginInstance
{
    std::unique_ptr<AudioPluginInstance> instance;
    PluginDescription description;
};

bool loadPlugin (const String& path, double sampleRate, int blockSize,
                 PluginInstance& result, String& error)
{
    const File file (path);

    if (! file.exists())
    {
        error = "file does not exist: " + path;
        return false;
    }

    OwnedArray<PluginDescription> descriptions;

    for (auto* fmt : formatManager().getFormats())
        if (fmt->fileMightContainThisPluginType (file.getFullPathName()))
            fmt->findAllTypesForFile (descriptions, file.getFullPathName());

    if (descriptions.isEmpty())
    {
        error = "no plugin class found in " + path;
        return false;
    }

    result.description = *descriptions[0];

    result.instance = formatManager().createPluginInstance (result.description, sampleRate, blockSize, error);

    if (result.instance == nullptr)
    {
        if (error.isEmpty())
            error = "unknown instantiation failure";
        return false;
    }

    // Ask for a plain stereo layout; reverbs are stereo in/out.
    AudioProcessor::BusesLayout layout;
    layout.inputBuses.add (AudioChannelSet::stereo());
    layout.outputBuses.add (AudioChannelSet::stereo());

    if (result.instance->getBusCount (true) == 1 && result.instance->getBusCount (false) == 1)
        result.instance->setBusesLayout (layout);

    result.instance->prepareToPlay (sampleRate, blockSize);
    result.instance->reset();

    return true;
}

//==============================================================================
/*
    Some VST3 controllers render parameter text from their own internal state
    rather than from the value handed to getParamStringByValue, so each probe has
    to be pushed into the plugin before its text is read.
*/
String describeParameter (AudioPluginInstance& instance, AudioProcessorParameter& parameter, int index)
{
    String paramID = "param" + String (index);
    String label;

    if (auto* withID = dynamic_cast<AudioProcessorParameterWithID*> (&parameter))
    {
        paramID = withID->paramID;
        label = withID->label;
    }
    else
    {
        label = parameter.getLabel();
    }

    double naturalMin = 0.0, naturalMax = 1.0, naturalDefault = 0.0;
    bool hasNaturalRange = false;

    if (auto* ranged = dynamic_cast<RangedAudioParameter*> (&parameter))
    {
        naturalMin      = (double) ranged->convertFrom0to1 (0.0f);
        naturalMax      = (double) ranged->convertFrom0to1 (1.0f);
        naturalDefault  = (double) ranged->convertFrom0to1 (parameter.getDefaultValue());
        hasNaturalRange = true;
    }

    const int numSteps = parameter.getNumSteps();
    const bool discrete = numSteps > 1 && numSteps <= 64;

    const int numProbes = discrete ? numSteps : 33;

    StringArray sampleTexts;
    StringArray distinctTexts;
    String naturalUnit;

    const float defaultNormalised = parameter.getDefaultValue();
    parameter.setValueNotifyingHost (defaultNormalised);
    const auto defaultText = parameter.getText (defaultNormalised, 64);

    for (int i = 0; i < numProbes; ++i)
    {
        const float normalised = (float) i / (float) (numProbes - 1);

        parameter.setValueNotifyingHost (normalised);

        const auto text = parameter.getText (normalised, 64);
        sampleTexts.add (text);

        if (! distinctTexts.contains (text))
        {
            distinctTexts.add (text);

            if (naturalUnit.isEmpty() && text.containsAnyOf ("ms %H z d B st"))
                naturalUnit = text;
        }
    }

    parameter.setValueNotifyingHost (defaultNormalised);
    instance.updateHostDisplay();

    String textArray;
    for (int i = 0; i < sampleTexts.size(); ++i)
        textArray << (i > 0 ? ", " : "") << jsonString (sampleTexts[i]);

    String distinctArray;
    for (int i = 0; i < distinctTexts.size(); ++i)
        distinctArray << (i > 0 ? ", " : "") << jsonString (distinctTexts[i]);

    String out;
    out << "    {";
    out << "\"index\": " << index;
    out << ", \"id\": " << jsonString (paramID);
    out << ", \"name\": " << jsonString (parameter.getName (128));
    out << ", \"label\": " << jsonString (label);
    out << ", \"numSteps\": " << numSteps;
    out << ", \"isBoolean\": " << (parameter.isBoolean() ? "true" : "false");
    out << ", \"default\": " << jsonNumber (parameter.getDefaultValue());
    out << ", \"defaultText\": " << jsonString (defaultText);

    if (hasNaturalRange)
    {
        out << ", \"naturalMin\": " << jsonNumber (naturalMin);
        out << ", \"naturalMax\": " << jsonNumber (naturalMax);
        out << ", \"naturalDefault\": " << jsonNumber (naturalDefault);
    }

    out << ", \"distinctCount\": " << distinctTexts.size();
    out << ", \"distinct\": [" << distinctArray << "]";
    out << ", \"texts\": [" << textArray << "]";
    out << "}";

    return out;
}

int doScan (const String& path)
{
    PluginInstance plugin;
    String error;

    if (! loadPlugin (path, 48000.0, 512, plugin, error))
    {
        std::printf ("{\"error\": %s}\n", jsonString (error).toRawUTF8());
        return 1;
    }

    auto& inst = *plugin.instance;

    String out;
    out << "{\n";
    out << "  \"file\": " << jsonString (path) << ",\n";
    out << "  \"name\": " << jsonString (plugin.description.name) << ",\n";
    out << "  \"vendor\": " << jsonString (plugin.description.manufacturerName) << ",\n";
    out << "  \"version\": " << jsonString (plugin.description.version) << ",\n";
    out << "  \"category\": " << jsonString (plugin.description.category) << ",\n";
    out << "  \"latencySamples\": " << inst.getLatencySamples() << ",\n";
    out << "  \"tailSeconds\": " << jsonNumber (inst.getTailLengthSeconds()) << ",\n";

    out << "  \"inputBuses\": [";
    for (int i = 0; i < inst.getBusCount (true); ++i)
        out << (i > 0 ? ", " : "") << jsonString (inst.getBus (true, i)->getName() + ":" + inst.getBus (true, i)->getCurrentLayout().getDescription());
    out << "],\n";

    out << "  \"outputBuses\": [";
    for (int i = 0; i < inst.getBusCount (false); ++i)
        out << (i > 0 ? ", " : "") << jsonString (inst.getBus (false, i)->getName() + ":" + inst.getBus (false, i)->getCurrentLayout().getDescription());
    out << "],\n";

    out << "  \"numPrograms\": " << inst.getNumPrograms() << ",\n";

    out << "  \"programNames\": [";
    const int maxPrograms = jmin (inst.getNumPrograms(), 400);
    for (int i = 0; i < maxPrograms; ++i)
        out << (i > 0 ? ", " : "") << jsonString (inst.getProgramName (i));
    out << "],\n";

    out << "  \"parameters\": [\n";

    const auto& params = inst.getParameters();
    for (int i = 0; i < params.size(); ++i)
    {
        out << describeParameter (inst, *params[i], i);
        out << (i + 1 < params.size() ? ",\n" : "\n");
    }

    out << "  ]\n}\n";

    std::printf ("%s", out.toRawUTF8());
    return 0;
}

//==============================================================================
struct RenderConfig
{
    String pluginPath;
    String outPath;
    String metaPath;
    String materialPath;                  // --in: process this file instead of a test signal
    double sampleRate = 48000.0;
    double seconds    = 20.0;
    int    blockSize  = 512;
    String test       = "impulse";
    int    numHits    = 1;
    double spacing    = 5.0;
    String channels   = "LR";
    double warmup     = 1.0;
    double sweepStart = 20.0;
    double sweepEnd   = 20000.0;
    double sineFreq   = 1000.0;
    double level      = 1.0;
    double smoothing  = 0.0;              // extra seconds of silence after param change
    std::vector<std::pair<String, String>> assignments;  // (paramID, normalized or text)
    std::vector<std::pair<String, String>> rawAssignments;
};

bool assignParameter (AudioPluginInstance& inst, const String& id, const String& value, bool natural, String& error)
{
    int index = 0;

    for (auto* param : inst.getParameters())
    {
        // Hosted VST3 parameters do not derive from AudioProcessorParameterWithID,
        // so fall back to the synthetic index-based id printed by "scan".
        const String paramID = dynamic_cast<AudioProcessorParameterWithID*> (param) != nullptr
                                 ? static_cast<AudioProcessorParameterWithID*> (param)->paramID
                                 : "param" + String (index);

        ++index;

        if (paramID != id && param->getName (128) != id)
            continue;

        const bool numeric = value.containsOnly ("0123456789.+-eE");

        if (! numeric)
        {
            const auto wanted = value.trim();

            // VST3 controllers parse the display string directly; verify the
            // round trip because not every controller implements it.
            const float parsed = param->getValueForText (wanted);
            param->setValueNotifyingHost (jlimit (0.0f, 1.0f, parsed));

            if (param->getText (parsed, 64).trim().equalsIgnoreCase (wanted))
                return true;

            const int steps = param->getNumSteps();

            if (steps > 1 && steps <= 512)
            {
                for (int s = 0; s < steps; ++s)
                {
                    const float norm = (float) s / (float) (steps - 1);
                    param->setValueNotifyingHost (norm);

                    if (param->getText (norm, 64).trim().equalsIgnoreCase (wanted))
                        return true;
                }
            }
            else
            {
                for (int s = 0; s <= 512; ++s)
                {
                    const float norm = (float) s / 512.0f;
                    param->setValueNotifyingHost (norm);

                    if (param->getText (norm, 64).trim().equalsIgnoreCase (wanted))
                        return true;
                }
            }

            error = "value text not found for " + id + ": " + value;
            return false;
        }

        float norm = (float) value.getDoubleValue();

        if (natural)
        {
            if (auto* ranged = dynamic_cast<RangedAudioParameter*> (param))
                norm = ranged->convertTo0to1 ((float) value.getDoubleValue());
            else
            {
                error = "parameter " + id + " has no natural range";
                return false;
            }
        }

        param->setValueNotifyingHost (jlimit (0.0f, 1.0f, norm));
        return true;
    }

    error = "parameter not found: " + id;
    return false;
}

int doRender (const RenderConfig& config)
{
    PluginInstance plugin;
    String error;

    if (! loadPlugin (config.pluginPath, config.sampleRate, config.blockSize, plugin, error))
    {
        std::fprintf (stderr, "ERROR: %s\n", error.toRawUTF8());
        return 1;
    }

    auto& inst = *plugin.instance;

    for (const auto& a : config.assignments)
        if (! assignParameter (inst, a.first, a.second, false, error))
        {
            std::fprintf (stderr, "ERROR: %s\n", error.toRawUTF8());
            return 2;
        }

    for (const auto& a : config.rawAssignments)
        if (! assignParameter (inst, a.first, a.second, true, error))
        {
            std::fprintf (stderr, "ERROR: %s\n", error.toRawUTF8());
            return 2;
        }

    inst.prepareToPlay (config.sampleRate, config.blockSize);
    inst.reset();

    const int numChannels = jmax (2, inst.getTotalNumOutputChannels());
    const double sr = config.sampleRate;

    const bool useMaterial = config.materialPath.isNotEmpty();

    // With --in, the source file is played first and --sec is the amount of
    // extra time rendered after it, so the tail is always captured in full.
    AudioBuffer<float> material;

    if (useMaterial)
    {
        AudioFormatManager formats;
        formats.registerBasicFormats();

        std::unique_ptr<AudioFormatReader> reader (formats.createReaderFor (File (config.materialPath)));

        if (reader == nullptr)
        {
            std::fprintf (stderr, "ERROR: cannot read material %s\n", config.materialPath.toRawUTF8());
            return 3;
        }

        const int length = (int) jmin ((int64_t) 1 << 30, reader->lengthInSamples);
        material.setSize (2, jmax (1, length));
        material.clear();
        reader->read (&material, 0, length, 0, true, true);

        if (reader->numChannels == 1)
            material.copyFrom (1, 0, material, 0, 0, length);

        if ((double) reader->sampleRate != sr)
            std::fprintf (stderr, "WARNING: material is %g Hz, rendering at %g Hz\n",
                          reader->sampleRate, sr);
    }

    const int64_t warmupSamples = (int64_t) std::llround (config.warmup * sr);
    const int64_t requestedSamples = (int64_t) std::llround (config.seconds * sr);
    const int64_t materialSamples = useMaterial ? (int64_t) material.getNumSamples() : 0;
    const int64_t subjectSamples = useMaterial ? materialSamples + requestedSamples : requestedSamples;
    const int64_t totalSamples = ((subjectSamples + config.blockSize - 1) / config.blockSize) * config.blockSize;

    // Absolute sample positions at which an impulse is injected.
    std::vector<int64_t> hitSamples;
    for (int h = 0; h < config.numHits; ++h)
        hitSamples.push_back (warmupSamples + (int64_t) std::llround (h * config.spacing * sr));

    const bool useLeft  = config.channels.containsChar ('L');
    const bool useRight = config.channels.containsChar ('R');

    auto generate = [&] (int64_t n, int channel) -> float
    {
        if (channel == 0 ? ! useLeft : ! useRight)
            return 0.0f;

        if (useMaterial)
        {
            if (n < 0 || n >= materialSamples)
                return 0.0f;

            return material.getSample (jmin (channel, material.getNumChannels() - 1), (int) n);
        }

        if (config.test == "impulse")
        {
            for (auto h : hitSamples)
                if (h == n)
                    return (float) config.level;

            return 0.0f;
        }

        if (n < warmupSamples)
            return 0.0f;

        const double t = (double) (n - warmupSamples) / sr;
        const double duration = (double) (totalSamples - warmupSamples) / sr;

        if (config.test == "sweep")
        {
            const double ratio = config.sweepEnd / config.sweepStart;
            const double k = std::log (ratio);
            const double phase = 2.0 * MathConstants<double>::pi * config.sweepStart * duration / k
                                   * (std::exp (t / duration * k) - 1.0);
            return (float) (config.level * std::sin (phase));
        }

        if (config.test == "sine")
            return (float) (config.level * std::sin (2.0 * MathConstants<double>::pi * config.sineFreq * t));

        if (config.test == "noise")
        {
            uint32_t x = (uint32_t) (n * 1664525u + (uint32_t) channel * 1013904223u + 12345u);
            x ^= x >> 13; x *= 2654435761u; x ^= x >> 16;
            return (float) (config.level * ((double) x / 2147483648.0 - 1.0));
        }

        return 0.0f;
    };

    AudioBuffer<float> buffer (jmax (2, inst.getTotalNumInputChannels()), config.blockSize);
    AudioBuffer<float> recorded (numChannels, (int) totalSamples);
    recorded.clear();

    MidiBuffer midi;
    float peak = 0.0f;
    double sumOfSquares = 0.0;

    for (int64_t pos = 0; pos < totalSamples; pos += config.blockSize)
    {
        buffer.clear();

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* dest = buffer.getWritePointer (ch);
            for (int i = 0; i < config.blockSize; ++i)
                dest[i] = generate (pos + i, jmin (ch, 1));
        }

        midi.clear();
        inst.processBlock (buffer, midi);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto* src = buffer.getReadPointer (jmin (ch, buffer.getNumChannels() - 1));
            auto* dest = recorded.getWritePointer (ch);
            for (int i = 0; i < config.blockSize; ++i)
            {
                const float v = src[i];
                dest[pos + i] = v;
                peak = jmax (peak, std::abs (v));
                sumOfSquares += (double) v * (double) v;
            }
        }
    }

    const File outFile (config.outPath);
    outFile.deleteFile();

    if (! writeFloatWav (outFile, recorded, sr))
    {
        std::fprintf (stderr, "ERROR: could not write %s\n", config.outPath.toRawUTF8());
        return 3;
    }

    const double rms = std::sqrt (sumOfSquares / (double) jmax<int64_t> (1, totalSamples * numChannels));

    String hitList;
    for (size_t i = 0; i < hitSamples.size(); ++i)
        hitList << (i > 0 ? "," : "") << hitSamples[i];

    String summary;
    summary << "{\"out\": " << jsonString (config.outPath)
            << ", \"material\": " << jsonString (config.materialPath)
            << ", \"sampleRate\": " << jsonNumber (sr, 2)
            << ", \"samples\": " << (long long) totalSamples
            << ", \"channels\": " << numChannels
            << ", \"latencySamples\": " << inst.getLatencySamples()
            << ", \"peak\": " << jsonNumber (peak)
            << ", \"rms\": " << jsonNumber (rms)
            << ", \"hits\": [" << hitList << "]}";

    if (config.metaPath.isNotEmpty())
        File (config.metaPath).replaceWithText (summary);

    std::printf ("%s\n", summary.toRawUTF8());

    return 0;
}

void printUsage()
{
    std::printf (
        "usage:\n"
        "  RvxAnalyzer bench [--sr 48000] [--bs 64] [--sec 8]\n"
        "  RvxAnalyzer switchtest\n"
        "  RvxAnalyzer dspcheck\n"
        "  RvxAnalyzer freezecheck\n"
        "  RvxAnalyzer cloudcheck\n"
        "  RvxAnalyzer layouttest <plugin.vst3>\n"
        "  RvxAnalyzer scan   <plugin.vst3>\n"
        "  RvxAnalyzer render <plugin.vst3> --out <file.wav> [--sr 48000] [--sec 20]\n"
        "                     [--bs 512] [--test impulse|sweep|noise|sine] [--hits 1]\n"
        "                     [--spacing 5.0] [--channels LR|L|R] [--warmup 1.0]\n"
        "                     [--set id=norm] [--setraw id=natural] [--level 1.0]\n"
        "  RvxAnalyzer render <plugin.vst3> --in <material.wav> [--sec 4] ...\n"
        "                     (--sec is the tail length rendered after the file)\n");
}

//==============================================================================
/** Runs the reverb engine in-process, with no VST3 wrapper and no host, so a
    fault can be attributed to the DSP rather than to the hosting layer. */
int doBench (const juce::StringArray& arguments)
{
    double sampleRate = 48000.0;
    int blockSize = 64;
    double seconds = 1.0;

    for (int i = 2; i < arguments.size(); ++i)
    {
        const juce::String arg = arguments[i];
        auto next = [&]() -> juce::String
        {
            return (i + 1 < arguments.size()) ? arguments[++i] : juce::String();
        };

        if      (arg == "--sr")  sampleRate = next().getDoubleValue();
        else if (arg == "--bs")  blockSize  = next().getIntValue();
        else if (arg == "--sec") seconds    = next().getDoubleValue();
    }

    const int64_t totalSamples = (int64_t) std::llround (seconds * sampleRate);

    for (int mode = 0; mode < rivage::kNumAlgorithms; ++mode)
    {
        std::printf ("%s ... ", rivage::algorithmName (mode));
        std::fflush (stdout);

        rivage::ReverbEngine engine;
        engine.prepare (sampleRate, blockSize);

        rivage::ReverbParameters params;
        params.algorithm = mode;
        params.mix       = 0.35f;
        params.decay     = 0.55f;
        params.size      = 0.55f;
        params.predelay  = 0.10f;
        params.tone      = 0.5f;
        params.mod       = 0.35f;
        params.width     = 0.5f;

        engine.setParameters (params);
        engine.snapParameters();

        juce::AudioBuffer<float> buffer (2, blockSize);
        float peak = 0.0f;
        int nonFinite = 0;

        const auto startTime = std::chrono::steady_clock::now();

        for (int64_t pos = 0; pos < totalSamples; pos += blockSize)
        {
            buffer.clear();

            if (pos == 0)
                buffer.setSample (0, 0, 1.0f);

            engine.process (buffer);

            for (int ch = 0; ch < 2; ++ch)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    const float v = buffer.getSample (ch, i);

                    if (! std::isfinite (v))
                        ++nonFinite;

                    peak = juce::jmax (peak, std::abs (v));
                }
            }
        }

        const auto elapsed = std::chrono::duration<double> (std::chrono::steady_clock::now() - startTime).count();
        const double audioSeconds = (double) totalSamples / sampleRate;
        const double realtimeFactor = elapsed > 0.0 ? audioSeconds / elapsed : 0.0;

        std::printf ("peak=%.4f nonfinite=%d  rt=%.1fx  cpu=%.2f%%\n",
                     peak, nonFinite, realtimeFactor, realtimeFactor > 0.0 ? 100.0 / realtimeFactor : 0.0);
        std::fflush (stdout);
    }

    std::printf ("bench ok\n");
    return 0;
}

/** Renders a sustained input while the algorithm changes underneath it and
    looks for a discontinuity at the switch. A seamless crossfade leaves the
    sample-to-sample delta at the switch indistinguishable from the tail. */
int doSwitchTest (const juce::StringArray& arguments)
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 64;
    constexpr double holdSeconds = 0.7;
    constexpr int switches = 12;

    rivage::ReverbEngine engine;
    engine.prepare (sampleRate, blockSize);

    rivage::ReverbParameters params;
    params.algorithm = 0;
    params.mix       = 0.85f;
    params.decay     = 0.62f;   // a big, still-ringing tail
    params.size      = 0.6f;
    params.predelay  = 0.05f;
    params.tone      = 0.5f;
    params.mod       = 0.4f;
    params.width     = 0.55f;

    engine.setParameters (params);
    engine.snapParameters();

    const int blockCount = (int) std::lround (holdSeconds * sampleRate / blockSize);
    const int totalBlocks = blockCount * switches;
    const int64_t totalSamples = (int64_t) totalBlocks * blockSize;

    std::vector<float> out ((size_t) totalSamples, 0.0f);
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::Random rng (12345);

    uint32_t state = 0x1234567u;

    for (int block = 0; block < totalBlocks; ++block)
    {
        buffer.clear();

        JUCE_BEGIN_IGNORE_WARNINGS_MSVC (4189)
        (void) state;
        JUCE_END_IGNORE_WARNINGS_MSVC

        // Plucked-guitar-like noise bursts: the tail is always ringing.
        if (block % 8 == 0)
        {
            for (int i = 0; i < blockSize; ++i)
            {
                state = state * 1664525u + 1013904223u;
                const float noise = ((float) ((state >> 9) & 0x3FFF) / 8192.0f - 1.0f) * 0.06f;
                buffer.setSample (0, i, noise);
                buffer.setSample (1, i, noise * 0.9f);
            }
        }

        if (block > 0 && block % blockCount == 0)
        {
            params.algorithm = (params.algorithm + 1) % rivage::kNumAlgorithms;
            engine.setParameters (params);
        }

        engine.process (buffer);

        for (int i = 0; i < blockSize; ++i)
            out[(size_t) (block * blockSize + i)] = 0.5f * (buffer.getSample (0, i) + buffer.getSample (1, i));
    }

    auto maxDelta = [] (const std::vector<float>& v, int64_t from, int64_t to)
    {
        float m = 0.0f;

        for (int64_t i = juce::jmax<int64_t> (1, from); i < juce::jmin<int64_t> ((int64_t) v.size(), to); ++i)
            m = juce::jmax (m, std::abs (v[(size_t) i] - v[(size_t) (i - 1)]));

        return m;
    };

    const int64_t switchSamples = (int64_t) std::lround (holdSeconds * sampleRate);
    const int64_t window = (int64_t) std::lround (0.010 * sampleRate);

    float worstAtSwitch = 0.0f;
    float worstElsewhere = 0.0f;

    for (int s = 1; s < switches; ++s)
    {
        const int64_t at = (int64_t) s * switchSamples;

        worstAtSwitch = juce::jmax (worstAtSwitch, maxDelta (out, at - window, at + window));
        worstElsewhere = juce::jmax (worstElsewhere,
                                     maxDelta (out, at - 4 * window, at - 2 * window));
    }

    const double ratioDb = worstAtSwitch > 0.0f && worstElsewhere > 0.0f
                             ? 20.0 * std::log10 (worstAtSwitch / worstElsewhere)
                             : 0.0;

    auto rmsWindow = [] (const std::vector<float>& v, int64_t from, int64_t to)
    {
        double sum = 0.0;
        int64_t count = 0;

        for (int64_t i = juce::jmax<int64_t> (0, from); i < juce::jmin<int64_t> ((int64_t) v.size(), to); ++i)
        {
            sum += (double) v[(size_t) i] * (double) v[(size_t) i];
            ++count;
        }

        return count > 0 ? std::sqrt (sum / (double) count) : 0.0;
    };

    for (int s = 1; s < switches; ++s)
    {
        const int64_t at = (int64_t) s * switchSamples;
        const double before = rmsWindow (out, at - window * 10, at - window * 2);
        const double after  = rmsWindow (out, at + window * 2, at + window * 10);

        std::printf ("  switch %2d -> %-7s rms %.5f -> %.5f\n", s,
                     rivage::algorithmName (s % rivage::kNumAlgorithms),
                     before, after);
    }

    std::printf ("switch delta peak=%.5f tail=%.5f  ratio=%+.1f dB\n",
                 worstAtSwitch, worstElsewhere, ratioDb);
    std::printf ("switchtest ok\n");
    return 0;
}

int doLayoutTest (const String& path)
{
    int failures = 0;
    for (int inputs : { 1, 2 })
        for (int outputs : { 1, 2 })
        {
            PluginInstance plugin;
            String error;
            if (! loadPlugin (path, 48000.0, 64, plugin, error))
            {
                std::fprintf (stderr, "%s\n", error.toRawUTF8());
                return 1;
            }
            auto& instance = *plugin.instance;
            instance.releaseResources();
            AudioProcessor::BusesLayout layout;
            layout.inputBuses.add (AudioChannelSet::canonicalChannelSet (inputs));
            layout.outputBuses.add (AudioChannelSet::canonicalChannelSet (outputs));
            const bool supported = instance.setBusesLayout (layout);
            bool passed = supported == (inputs <= outputs);
            if (supported)
            {
                passed = assignParameter (instance, "Mix", "0", false, error) && passed;
                for (double rate : { 44100.0, 48000.0, 96000.0 })
                {
                    // VST3's setupProcessing declares the maximum block size.
                    instance.prepareToPlay (rate, 1024);
                    for (int samples : { 0, 1, 32, 64, 257, 1024 })
                    {
                        AudioBuffer<float> buffer (outputs, samples);
                        MidiBuffer midi;
                        buffer.clear();
                        for (int i = 0; i < samples; ++i)
                        {
                            buffer.setSample (0, i, 0.125f);
                            if (inputs == 2) buffer.setSample (1, i, -0.25f);
                        }
                        instance.processBlock (buffer, midi);
                        for (int ch = 0; ch < outputs; ++ch)
                            for (int i = 0; i < samples; ++i)
                                passed = passed && buffer.getSample (ch, i)
                                    == (ch == 1 && inputs == 2 ? -0.25f : 0.125f);
                    }
                    instance.releaseResources();
                }
            }
            std::printf ("%s VST3 layout %d -> %d\n", passed ? "PASS" : "FAIL", inputs, outputs);
            failures += passed ? 0 : 1;
        }
    return failures == 0 ? 0 : 1;
}

} // namespace

//==============================================================================
int main (int argc, char** argv)
{
    ScopedJuceInitialiser_GUI juceInitialiser;

   #if JUCE_WINDOWS
    ::SetConsoleOutputCP (CP_UTF8);
   #endif

   #if JUCE_DEBUG && JUCE_MSVC
    // Full debug-heap validation: every allocation carries guard bytes and the
    // allocator re-checks the whole heap on every call, so the first write past
    // the end of a block is reported at the instruction that does it.
    _CrtSetReportMode (_CRT_WARN,   _CRTDBG_MODE_FILE);
    _CrtSetReportFile (_CRT_WARN,   _CRTDBG_FILE_STDERR);
    _CrtSetReportMode (_CRT_ERROR,  _CRTDBG_MODE_FILE);
    _CrtSetReportFile (_CRT_ERROR,  _CRTDBG_FILE_STDERR);
    _CrtSetReportMode (_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile (_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetDbgFlag (_CRTDBG_ALLOC_MEM_DF | _CRTDBG_CHECK_ALWAYS_DF
                     | _CRTDBG_CHECK_CRT_DF | _CRTDBG_DELAY_FREE_MEM_DF);
   #endif

    const auto arguments = getCommandLineArguments (argc, argv);

    if (arguments.size() >= 2 && arguments[1] == "bench")
        return doBench (arguments);

    if (arguments.size() >= 2 && arguments[1] == "switchtest")
        return doSwitchTest (arguments);

    if (arguments.size() >= 2 && arguments[1] == "dspcheck")
        return rivage::runDspChecks();

    if (arguments.size() >= 2 && arguments[1] == "freezecheck")
        return rivage::runFreezeChecks();

    if (arguments.size() >= 2 && arguments[1] == "cloudcheck")
    {
       #if JUCE_DEBUG && JUCE_MSVC
        // This checks delay bounds, not allocator corruption. The full heap
        // walk on every allocation makes the high-rate buffer matrix costly.
        _CrtSetDbgFlag (_CRTDBG_ALLOC_MEM_DF);
       #endif
        return rivage::runCloudRangeChecks();
    }

    if (arguments.size() < 3)
    {
        printUsage();
        return -1;
    }

    const String command = arguments[1];
    const String path    = arguments[2];

    if (command == "layouttest")
        return doLayoutTest (path);

    if (command == "scan")
        return doScan (path);

    if (command != "render")
    {
        printUsage();
        return -1;
    }

    RenderConfig config;
    config.pluginPath = path;

    for (int i = 3; i < arguments.size(); ++i)
    {
        const String arg = arguments[i];

        auto next = [&]() -> String { return (i + 1 < arguments.size()) ? arguments[++i] : String(); };

        if      (arg == "--out")        config.outPath    = next();
        else if (arg == "--meta")       config.metaPath   = next();
        else if (arg == "--in")         config.materialPath = next();
        else if (arg == "--sr")         config.sampleRate = next().getDoubleValue();
        else if (arg == "--sec")        config.seconds    = next().getDoubleValue();
        else if (arg == "--bs")         config.blockSize  = next().getIntValue();
        else if (arg == "--test")       config.test       = next();
        else if (arg == "--hits")       config.numHits    = next().getIntValue();
        else if (arg == "--spacing")    config.spacing    = next().getDoubleValue();
        else if (arg == "--channels")   config.channels   = next();
        else if (arg == "--warmup")     config.warmup     = next().getDoubleValue();
        else if (arg == "--level")      config.level      = next().getDoubleValue();
        else if (arg == "--sine")       config.sineFreq   = next().getDoubleValue();
        else if (arg == "--sweep-end")  config.sweepEnd   = next().getDoubleValue();
        else if (arg == "--set")
        {
            const auto pair = next();
            const auto id = pair.upToFirstOccurrenceOf ("=", false, false);
            const auto value = pair.fromFirstOccurrenceOf ("=", false, false);
            config.assignments.emplace_back (id, value);
        }
        else if (arg == "--setraw")
        {
            const auto pair = next();
            const auto id = pair.upToFirstOccurrenceOf ("=", false, false);
            const auto value = pair.fromFirstOccurrenceOf ("=", false, false);
            config.rawAssignments.emplace_back (id, value);
        }
    }

    if (config.outPath.isEmpty())
        config.outPath = File::getCurrentWorkingDirectory()
                             .getChildFile ("ir.wav").getFullPathName();

    return doRender (config);
}
