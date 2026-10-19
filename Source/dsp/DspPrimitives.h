#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cmath>
#include <vector>

namespace rivage
{

//==============================================================================
/** Control mapping shared by every algorithm: a 0..1 knob becomes an RT60 in
    seconds with the exponential taper the guitar reverb family uses. */
inline float decaySecondsFromNormalised (float normalised) noexcept
{
    return 0.15f * std::pow (400.0f, juce::jlimit (0.0f, 1.0f, normalised));
}

inline float normalisedFromDecaySeconds (float seconds) noexcept
{
    return std::log (juce::jlimit (0.15f, 60.0f, seconds) / 0.15f) / std::log (400.0f);
}

/** Per-sample loop gain that turns a delay of `delaySamples` into an RT60. */
inline float loopGainForRt60 (float delaySamples, double sampleRate, float rt60Seconds) noexcept
{
    const float safeRt = juce::jmax (0.05f, rt60Seconds);
    const float exponent = -3.0f * delaySamples / ((float) sampleRate * safeRt);
    return juce::jlimit (0.0f, 0.99995f, std::pow (10.0f, exponent));
}

//==============================================================================
/** Fractional delay line with 4-point cubic interpolation. Every read is
    clamped, so a non-finite or out-of-range request can never build a bad index. */
struct DelayLine
{
    void prepare (int maxSamples)
    {
        buffer.assign ((size_t) juce::jmax (8, maxSamples), 0.0f);
        writeIndex = 0;
    }

    void clear()
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writeIndex = 0;
    }

    inline void push (float value) noexcept
    {
        buffer[(size_t) writeIndex] = value;

        if (++writeIndex >= (int) buffer.size())
            writeIndex = 0;
    }

    inline float read (float delay) const noexcept
    {
        const int size = (int) buffer.size();

        if (size < 8)
            return 0.0f;

        if (! std::isfinite (delay))
            delay = 2.0f;

        delay = juce::jlimit (2.0f, (float) (size - 3), delay);

        float position = (float) writeIndex - delay;
        position = std::fmod (position, (float) size);

        if (position < 0.0f)
            position += (float) size;

        const int index = (int) position;
        const float fraction = position - (float) index;

        const int i0 = (index - 1 + size) % size;
        const int i1 = index;
        const int i2 = (index + 1) % size;
        const int i3 = (index + 2) % size;

        const float y0 = buffer[(size_t) i0];
        const float y1 = buffer[(size_t) i1];
        const float y2 = buffer[(size_t) i2];
        const float y3 = buffer[(size_t) i3];

        const float c0 = y1;
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);

        return ((c3 * fraction + c2) * fraction + c1) * fraction + c0;
    }

    std::vector<float> buffer;
    int writeIndex = 0;
};

//==============================================================================
/** Schroeder allpass on top of the interpolated delay line. */
struct Allpass
{
    void prepare (int maxSamples, float coefficient, int delaySamples)
    {
        line.prepare (maxSamples);
        gain = coefficient;
        delay = (float) juce::jmax (1, delaySamples);
    }

    void clear() { line.clear(); }

    inline float process (float input) noexcept
    {
        const float delayed = line.read (delay);
        const float v = input + gain * delayed;
        line.push (v);
        return delayed - gain * v;
    }

    void setDelay (float samples) noexcept { delay = juce::jmax (1.0f, samples); }
    void setGain (float g) noexcept        { gain = juce::jlimit (-0.95f, 0.95f, g); }

    DelayLine line;
    float gain = 0.5f;
    float delay = 1.0f;
};

//==============================================================================
/** RBJ biquad in transposed direct form II. */
struct Biquad
{
    void reset() noexcept { z1 = z2 = 0.0f; }

    inline float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void setLowPass (double sampleRate, double frequency, double q) noexcept
    {
        const double w0 = omega (sampleRate, frequency);
        const double cosw = std::cos (w0);
        const double alpha = std::sin (w0) / (2.0 * juce::jmax (0.05, q));
        const double a0 = 1.0 + alpha;

        b0 = (float) (((1.0 - cosw) * 0.5) / a0);
        b1 = (float) ((1.0 - cosw) / a0);
        b2 = b0;
        a1 = (float) (-2.0 * cosw / a0);
        a2 = (float) ((1.0 - alpha) / a0);
    }

    void setHighPass (double sampleRate, double frequency, double q) noexcept
    {
        const double w0 = omega (sampleRate, frequency);
        const double cosw = std::cos (w0);
        const double alpha = std::sin (w0) / (2.0 * juce::jmax (0.05, q));
        const double a0 = 1.0 + alpha;

        b0 = (float) (((1.0 + cosw) * 0.5) / a0);
        b1 = (float) (-(1.0 + cosw) / a0);
        b2 = b0;
        a1 = (float) (-2.0 * cosw / a0);
        a2 = (float) ((1.0 - alpha) / a0);
    }

    /** RBJ peaking EQ. */
    void setPeak (double sampleRate, double frequency, double q, double gainDb) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w0 = omega (sampleRate, frequency);
        const double cosw = std::cos (w0);
        const double alpha = std::sin (w0) / (2.0 * juce::jmax (0.05, q));
        const double a0 = 1.0 + alpha / A;

        b0 = (float) ((1.0 + alpha * A) / a0);
        b1 = (float) ((-2.0 * cosw) / a0);
        b2 = (float) ((1.0 - alpha * A) / a0);
        a1 = (float) ((-2.0 * cosw) / a0);
        a2 = (float) ((1.0 - alpha / A) / a0);
    }

    /** RBJ high shelf; slope 1 is the neutral setting. */
    void setHighShelf (double sampleRate, double frequency, double gainDb, double slope) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w0 = omega (sampleRate, frequency);
        const double cosw = std::cos (w0);
        const double sinw = std::sin (w0);
        const double alpha = sinw * 0.5 * std::sqrt ((A + 1.0 / A) * (1.0 / juce::jmax (0.1, slope) - 1.0) + 2.0);
        const double twoSqrtAAlpha = 2.0 * std::sqrt (A) * alpha;
        const double a0 = (A + 1.0) - (A - 1.0) * cosw + twoSqrtAAlpha;

        b0 = (float) ((A * ((A + 1.0) + (A - 1.0) * cosw + twoSqrtAAlpha)) / a0);
        b1 = (float) ((-2.0 * A * ((A - 1.0) + (A + 1.0) * cosw)) / a0);
        b2 = (float) ((A * ((A + 1.0) + (A - 1.0) * cosw - twoSqrtAAlpha)) / a0);
        a1 = (float) ((2.0 * ((A - 1.0) - (A + 1.0) * cosw)) / a0);
        a2 = (float) (((A + 1.0) - (A - 1.0) * cosw - twoSqrtAAlpha) / a0);
    }

    /** RBJ low shelf; slope 1 is the neutral setting. */
    void setLowShelf (double sampleRate, double frequency, double gainDb, double slope) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w0 = omega (sampleRate, frequency);
        const double cosw = std::cos (w0);
        const double sinw = std::sin (w0);
        const double alpha = sinw * 0.5 * std::sqrt ((A + 1.0 / A) * (1.0 / juce::jmax (0.1, slope) - 1.0) + 2.0);
        const double twoSqrtAAlpha = 2.0 * std::sqrt (A) * alpha;
        const double a0 = (A + 1.0) + (A - 1.0) * cosw + twoSqrtAAlpha;

        b0 = (float) ((A * ((A + 1.0) - (A - 1.0) * cosw + twoSqrtAAlpha)) / a0);
        b1 = (float) ((2.0 * A * ((A - 1.0) - (A + 1.0) * cosw)) / a0);
        b2 = (float) ((A * ((A + 1.0) - (A - 1.0) * cosw - twoSqrtAAlpha)) / a0);
        a1 = (float) ((-2.0 * ((A - 1.0) + (A + 1.0) * cosw)) / a0);
        a2 = (float) (((A + 1.0) + (A - 1.0) * cosw - twoSqrtAAlpha) / a0);
    }

    static double omega (double sampleRate, double frequency) noexcept
    {
        return 2.0 * juce::MathConstants<double>::pi
                 * juce::jlimit (10.0, sampleRate * 0.49, frequency) / sampleRate;
    }

    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;
};

//==============================================================================
/** One-pole low pass; also used as the splitter of the three-band absorption. */
struct OnePole
{
    void setCutoff (double sampleRate, double frequency) noexcept
    {
        coefficient = (float) (1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi
                                                * juce::jlimit (1.0, sampleRate * 0.49, frequency) / sampleRate));
    }

    inline float process (float x) noexcept
    {
        state += coefficient * (x - state);
        return state;
    }

    void clear() noexcept { state = 0.0f; }

    float coefficient = 0.3f;
    float state = 0.0f;
};

//==============================================================================
/** Sine table held as a member so no algorithm owns a static allocation. */
struct SineTable
{
    void prepare()
    {
        for (int i = 0; i <= size; ++i)
            table[(size_t) i] = std::sin (juce::MathConstants<float>::twoPi * (float) i / (float) size);
    }

    inline float lookup (float phase01) const noexcept
    {
        float p = phase01 - std::floor (phase01);
        const float position = p * (float) size;
        const int index = juce::jlimit (0, size - 1, (int) position);
        const float fraction = position - (float) index;

        return table[(size_t) index] + fraction * (table[(size_t) index + 1] - table[(size_t) index]);
    }

    static constexpr int size = 1024;
    std::array<float, size + 1> table {};
};

//==============================================================================
inline void hadamard8 (float* v) noexcept
{
    for (int step = 1; step < 8; step <<= 1)
    {
        for (int i = 0; i < 8; i += step << 1)
        {
            float* a = v + i;
            float* b = a + step;

            for (int j = 0; j < step; ++j)
            {
                const float x = a[j];
                const float y = b[j];
                a[j] = x + y;
                b[j] = x - y;
            }
        }
    }

    constexpr float normalise = 0.35355339f; // 1 / sqrt(8)

    for (int i = 0; i < 8; ++i)
        v[i] *= normalise;
}

inline void hadamard16 (float* v) noexcept
{
    for (int step = 1; step < 16; step <<= 1)
    {
        for (int i = 0; i < 16; i += step << 1)
        {
            float* a = v + i;
            float* b = a + step;

            for (int j = 0; j < step; ++j)
            {
                const float x = a[j];
                const float y = b[j];
                a[j] = x + y;
                b[j] = x - y;
            }
        }
    }

    for (int i = 0; i < 16; ++i)
        v[i] *= 0.25f;
}

//==============================================================================
/** Two-head granular pitch shifter, used only by SHIMMER's feedback path. */
class PitchShifter
{
public:
    void prepare (double sampleRate)
    {
        const int newSize = juce::nextPowerOfTwo ((int) (sampleRate * 0.14));
        buffer.assign ((size_t) newSize, 0.0f);
        writeIndex = 0;
        halfSize = juce::jmax (2, newSize / 2);
        phase = 0.0f;
    }

    void reset()
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writeIndex = 0;
        phase = 0.0f;
    }

    void setRatio (float r) noexcept { ratio = juce::jlimit (0.25f, 4.0f, r); }

    inline float process (float input) noexcept
    {
        const int size = (int) buffer.size();

        if (size < 8)
            return 0.0f;

        buffer[(size_t) writeIndex] = input;

        if (++writeIndex >= size)
            writeIndex = 0;

        phase += (1.0f - ratio) / (float) halfSize;

        if (phase >= 1.0f) phase -= 1.0f;
        if (phase < 0.0f)  phase += 1.0f;

        const float p2 = phase >= 0.5f ? phase - 0.5f : phase + 0.5f;
        const float g1 = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * phase);

        return g1 * readAt (2.0f + phase * (float) halfSize)
             + (1.0f - g1) * readAt (2.0f + p2 * (float) halfSize);
    }

private:
    inline float readAt (float delay) const noexcept
    {
        const int size = (int) buffer.size();

        if (size < 8)
            return 0.0f;

        if (! std::isfinite (delay))
            delay = 2.0f;

        delay = juce::jlimit (1.0f, (float) (size - 2), delay);

        float position = (float) writeIndex - delay;

        if (position < 0.0f)   position += (float) size;
        if (position >= (float) size) position -= (float) size;

        const int index = (int) position;
        const float fraction = position - (float) index;
        const int i1 = index % size;
        const int i2 = (index + 1) % size;

        return buffer[(size_t) i1] + fraction * (buffer[(size_t) i2] - buffer[(size_t) i1]);
    }

    std::vector<float> buffer;
    int writeIndex = 0;
    int halfSize = 1024;
    float phase = 0.0f;
    float ratio = 2.0f;
};

//==============================================================================
/** Small deterministic noise source used to build decorrelated tap patterns. */
struct Lcg
{
    explicit Lcg (uint32_t seed) noexcept : state (seed) {}

    inline float next() noexcept
    {
        state = state * 1664525u + 1013904223u;
        return (float) ((state >> 8) & 0x00FFFFFFu) / (float) 0x00FFFFFFu;
    }

    inline float signedUnit() noexcept
    {
        return next() > 0.5f ? 1.0f : -1.0f;
    }

    uint32_t state;
};

} // namespace rivage
