#pragma once

#include "dsp/ReverbEngine.h"

#include <limits>

namespace rivage
{

inline int runDspChecks()
{
    juce::ScopedNoDenormals noDenormals;
    int failures = 0;
    auto check = [&] (bool passed, const char* name)
    {
        std::printf ("%s %s\n", passed ? "PASS" : "FAIL", name);
        failures += passed ? 0 : 1;
    };

    // Compare against the original cubic reader across non-power-of-two sizes,
    // wrap boundaries and invalid delay requests. Interpolation must be exact.
    bool delaysMatch = true;
    juce::Random random (1947);
    for (int size : { 8, 9, 31, 128, 1447, 19264 })
    {
        DelayLine line;
        line.prepare (size);
        for (auto& value : line.buffer)
            value = random.nextFloat() - 0.5f;
        for (int i = 0; i < 20000; ++i)
        {
            line.writeIndex = random.nextInt (size);
            float delay = random.nextFloat() * (float) size * 1.3f - 3.0f;
            if (i == 0) delay = std::numeric_limits<float>::quiet_NaN();
            if (i == 1) delay = std::numeric_limits<float>::infinity();
            if (i == 2)
            {
                line.writeIndex = 2;
                delay = std::nextafter (2.0f, 3.0f);
            }
            const float safeDelay = juce::jlimit (2.0f, (float) (size - 3),
                                                std::isfinite (delay) ? delay : 2.0f);
            float position = std::fmod ((float) line.writeIndex - safeDelay, (float) size);
            if (position < 0.0f) position += (float) size;
            const int index = (int) position;
            const float fraction = position - (float) index;
            const float y0 = line.buffer[(size_t) ((index - 1 + size) % size)];
            const float y1 = line.buffer[(size_t) (index % size)];
            const float y2 = line.buffer[(size_t) ((index + 1) % size)];
            const float y3 = line.buffer[(size_t) ((index + 2) % size)];
            const float c1 = 0.5f * (y2 - y0);
            const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
            const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
            const float expected = ((c3 * fraction + c2) * fraction + c1) * fraction + y1;
            delaysMatch = delaysMatch && line.read (delay) == expected;
        }
    }
    check (delaysMatch, "cubic delay matches original reader (120000 cases)");

    juce::SmoothedValue<float> ramp;
    ramp.reset (1000.0, 0.1);
    ramp.setCurrentAndTargetValue (0.0f);
    ramp.setTargetValue (1.0f);
    bool rampCorrect = true;
    for (int step = 0; step < 4; ++step)
    {
        rampCorrect = advanceSmoothers (32, { &ramp }) && rampCorrect;
        rampCorrect = rampCorrect && std::abs (ramp.getCurrentValue()
            - juce::jmin (1.0f, (float) (step + 1) * 0.32f)) < 1.0e-6f;
    }
    check (rampCorrect && ! advanceSmoothers (32, { &ramp }),
           "control ramps reach final value and stop requesting coefficient updates");

    // Each knob must affect a running machine without a snap or mode change.
    // Two otherwise identical machines expose frozen ramps immediately.
    using Control = float AlgorithmParams::*;
    const Control controls[] = { &AlgorithmParams::decay, &AlgorithmParams::size,
                                 &AlgorithmParams::mod, &AlgorithmParams::freeze,
                                 &AlgorithmParams::drive };
    const char* names[] = { "decay", "size", "mod", "freeze", "drive" };
    for (int mode = 0; mode < kNumEngines; ++mode)
        for (int control = 0; control < 5; ++control)
        {
            if (control == 4 && mode != kSpringTankEngine) continue;
            if (control == 2 && (mode == kSpringAlgorithmIndex || mode == kSpringTankEngine)) continue;
            auto changed = createAlgorithm (mode);
            auto unchanged = createAlgorithm (mode);
            AlgorithmParams params;
            changed->prepare (48000.0);
            unchanged->prepare (48000.0);
            changed->setParams (params);
            unchanged->setParams (params);
            changed->snap();
            unchanged->snap();
            params.*controls[control] = 1.0f;
            changed->setParams (params);
            double difference = 0.0;
            bool finite = true;
            for (int i = 0; i < 16000; ++i)
            {
                const float input = 0.06f * std::sin ((float) i * 0.057f)
                                  + 0.04f * std::sin ((float) i * 0.131f);
                float aL, aR, bL, bR;
                changed->processSample (input, input * 0.7f, aL, aR);
                unchanged->processSample (input, input * 0.7f, bL, bR);
                finite = finite && std::isfinite (aL) && std::isfinite (aR);
                difference += std::abs (aL - bL) + std::abs (aR - bR);
            }
            const auto label = juce::String (algorithmName (mode)) + " live " + names[control];
            check (finite && difference > 1.0e-5, label.toRawUTF8());
        }

    // A live Tone edit must retain the old EQ until the next control tick,
    // then audibly move towards the target instead of staying frozen.
    {
        ReverbEngine edited, reference;
        edited.prepare (48000.0, 64);
        reference.prepare (48000.0, 64);
        ReverbParameters params;
        params.mix = 1.0f;
        edited.setParameters (params);
        reference.setParameters (params);
        edited.snapParameters();
        reference.snapParameters();
        juce::AudioBuffer<float> a (2, 8192), b (2, 8192);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 8192; ++i)
            {
                const float x = 0.1f * std::sin ((float) i * 0.04f);
                a.setSample (ch, i, x);
                b.setSample (ch, i, x);
            }
        edited.process (a);
        reference.process (b);
        params.tone = 1.0f;
        edited.setParameters (params);
        a.clear();
        b.clear();
        edited.process (a);
        reference.process (b);
        bool unchangedBeforeTick = true;
        double difference = 0.0;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 8192; ++i)
            {
                const float delta = std::abs (a.getSample (ch, i) - b.getSample (ch, i));
                if (i < 31) unchangedBeforeTick = unchangedBeforeTick && delta == 0.0f;
                difference += delta;
            }
        check (unchangedBeforeTick && difference > 1.0e-4, "live Tone starts at control tick and changes EQ");
    }

    // Mono must equal the mean of both stereo channels, even during automation
    // and when the host's block size changes. Also verify reset clears EQ state.
    for (int mode = 0; mode < kNumEngines; ++mode)
    {
        ReverbEngine mono, stereo;
        mono.prepare (48000.0, 64);
        stereo.prepare (48000.0, 64);
        ReverbParameters params;
        params.algorithm = mode == kSpringTankEngine ? kSpringAlgorithmIndex : mode;
        params.springModel = mode == kSpringTankEngine ? 1 : 0;
        mono.setParameters (params);
        stereo.setParameters (params);
        mono.snapParameters();
        stereo.snapParameters();
        bool matches = true;
        for (int block = 0; block < 120; ++block)
        {
            const int count = block % 3 == 0 ? 1 : (block % 3 == 1 ? 64 : 257);
            juce::AudioBuffer<float> m (1, count), s (2, count);
            for (int i = 0; i < count; ++i)
            {
                const float input = random.nextFloat() * 0.08f - 0.04f;
                m.setSample (0, i, input);
                s.setSample (0, i, input);
                s.setSample (1, i, input);
            }
            params.tone = block < 30 ? 0.1f : 0.9f;
            mono.setParameters (params);
            stereo.setParameters (params);
            mono.process (m);
            stereo.process (s);
            for (int i = 0; i < count; ++i)
                matches = matches && std::abs (m.getSample (0, i)
                    - 0.5f * (s.getSample (0, i) + s.getSample (1, i))) < 2.0e-7f;
        }
        const auto label = juce::String (algorithmName (mode)) + " mono fold-down";
        check (matches, label.toRawUTF8());
        stereo.reset();
        juce::AudioBuffer<float> silence (2, 512);
        silence.clear();
        stereo.process (silence);
        check (silence.getMagnitude (0, 512) == 0.0f, "reset clears tail and EQ history");
    }
    std::printf ("dspcheck: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

} // namespace rivage
