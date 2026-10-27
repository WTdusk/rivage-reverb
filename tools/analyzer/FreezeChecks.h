#pragma once

#include "dsp/ReverbEngine.h"
#include <cstdio>

namespace rivage
{

inline int runCloudRangeChecks()
{
    juce::ScopedNoDenormals noDenormals;
    int failures = 0;
    for (double rate : { 32000.0, 44100.0, 48000.0, 96000.0, 192000.0 })
        for (float size : { 0.0f, 0.5f, 1.0f })
            for (float mod : { 0.0f, 0.5f, 1.0f })
            {
                auto cloud = createAlgorithm (5);
                cloud->prepare (rate);
                AlgorithmParams params;
                params.size = size;
                params.mod = mod;
                params.decay = 1.0f;
                cloud->setParams (params);
                cloud->snap();
                bool finite = true;
                // Capacity is checked at snap and every control tick. Keep the
                // checked-iterator Debug run short; Release exercises the tail.
               #if JUCE_DEBUG
                constexpr int samples = 128;
               #else
                const int samples = (int) (rate * 0.25);
               #endif
                for (int i = 0; i < samples; ++i)
                {
                    float left, right;
                    cloud->processSample (i == 0 ? 1.0f : 0.0f, 0.0f, left, right);
                    finite = finite && std::isfinite (left) && std::isfinite (right)
                                    && std::abs (left) <= 1.0f && std::abs (right) <= 1.0f;
                }
                // Debug builds additionally assert every requested diffuser
                // delay fits its actual allocation, before read() can clamp it.
                std::printf ("%s CLOUD %.0f Hz size=%.1f mod=%.1f\n",
                             finite ? "PASS" : "FAIL", rate, size, mod);
                failures += finite ? 0 : 1;
            }
    std::printf ("cloudcheck: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

inline int runFreezeChecks()
{
    juce::ScopedNoDenormals noDenormals;
    int failures = 0;
    for (double rate : { 44100.0, 48000.0, 96000.0 })
        for (int mode = 0; mode < kNumEngines; ++mode)
        {
            auto silentInput = createAlgorithm (mode);
            auto loudInput = createAlgorithm (mode);
            AlgorithmParams params;
            params.decay = 0.0f; // Freeze must work independently of Decay.
            params.size = 0.6f;
            params.mod = 0.7f;
            for (auto* machine : { silentInput.get(), loudInput.get() })
            {
                machine->prepare (rate);
                machine->setParams (params);
                machine->snap();
            }
            juce::Random random (12345);
            const int exciteSamples = (int) (rate * 0.5);
            for (int i = 0; i < exciteSamples; ++i)
            {
                const float input = (random.nextFloat() - 0.5f) * 0.08f;
                float aL, aR, bL, bR;
                silentInput->processSample (input, input, aL, aR);
                loudInput->processSample (input, input, bL, bR);
            }
            params.freeze = 1.0f;
            silentInput->setParams (params);
            loudInput->setParams (params);
            double earlyEnergy = 0.0, lateEnergy = 0.0, inputDifference = 0.0;
            bool finite = true;
            const int total = (int) (rate * 6.0);
            for (int i = 0; i < total; ++i)
            {
                // New input begins after every parameter ramp has completed.
                const float input = i > (int) (rate * 0.15)
                    ? 0.2f * std::sin ((float) i * (float) (1800.0 / rate)) : 0.0f;
                float aL, aR, bL, bR;
                silentInput->processSample (0.0f, 0.0f, aL, aR);
                loudInput->processSample (input, -input * 0.7f, bL, bR);
                finite = finite && std::isfinite (aL) && std::isfinite (aR)
                                && std::isfinite (bL) && std::isfinite (bR);
                inputDifference += std::abs (aL - bL) + std::abs (aR - bR);
                const double energy = (double) aL * aL + (double) aR * aR;
                if (i >= (int) rate && i < (int) (rate * 2.0)) earlyEnergy += energy;
                if (i >= (int) (rate * 5.0)) lateEnergy += energy;
            }
            const double driftDb = 10.0 * std::log10 (juce::jmax (1.0e-30, lateEnergy)
                                                    / juce::jmax (1.0e-30, earlyEnergy));
            const bool held = finite && earlyEnergy > 1.0e-10 && lateEnergy > 1.0e-10
                                    && std::abs (driftDb) < 3.0 && inputDifference == 0.0;
            std::printf ("%s %-11s %.0f Hz freeze drift=%+.2f dB inputDiff=%.8g\n",
                         held ? "PASS" : "FAIL", algorithmName (mode), rate, driftDb, inputDifference);
            failures += held ? 0 : 1;

            // Unfreeze must restore a decaying tail, not leave unity feedback.
            params.freeze = 0.0f;
            silentInput->setParams (params);
            double releaseStart = 0.0, releaseEnd = 0.0;
            for (int i = 0; i < total; ++i)
            {
                float left, right;
                silentInput->processSample (0.0f, 0.0f, left, right);
                finite = finite && std::isfinite (left) && std::isfinite (right);
                const double energy = (double) left * left + (double) right * right;
                if (i < (int) rate) releaseStart += energy;
                if (i >= (int) (rate * 5.0)) releaseEnd += energy;
            }
            const bool released = finite && releaseEnd < releaseStart * 0.01;
            std::printf ("%s %-11s %.0f Hz unfreeze restores decay\n",
                         released ? "PASS" : "FAIL", algorithmName (mode), rate);
            failures += released ? 0 : 1;
        }
    std::printf ("freezecheck: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

} // namespace rivage
