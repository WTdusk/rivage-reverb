#pragma once

#include "ReverbAlgorithm.h"

namespace rivage
{
namespace spring
{

//==============================================================================
// Two spring tanks share one linear waveguide: a set of six coiled delay loops
// with a cascade of first order allpasses inside each loop. PURE runs that
// structure on its own. TANK puts the physical chain around it: a drive
// transducer in front, a level dependent damper in the loop, impact events
// injected into the coils and a pickup that saturates. Everything nonlinear is
// inside `if constexpr (kTank)`, so the PURE instantiation compiles to exactly
// the operations the old SpringAlgorithm performed, sample for sample.

inline constexpr int kSprings    = 6;
inline constexpr int kDispersion = 24;
inline constexpr int kChunk      = 32;

// Six coils of different length, like a real spring tank.
inline constexpr float kCoilMs[kSprings] = { 21.7f, 26.3f, 31.1f, 35.9f, 41.3f, 47.1f };

// The dispersion coefficient of a spring rises with frequency; a 24 stage
// cascade of first order allpasses turns a click into a descending chirp.
inline constexpr float kDispersionGain = 0.62f;
inline constexpr float kOutputTrim     = 0.44896f;

//==============================================================================
// TANK constants. They are deliberately few: the character is set by two
// numbers (gain into the transducer and the loop damping) plus the level of the
// material itself. There is no user trim for the impact layer on purpose.

inline constexpr float kTransducerKnee   = 0.9f;   // tanh knee of the transducer
inline constexpr float kTransducerBias   = 0.08f;  // static bias -> even harmonics
inline constexpr float kDriveRangeDb     = 18.0f;  // +-18 dB at the ends of Drive
inline constexpr float kDriveCompensation = 0.70f; // how much drive gain is undone
inline constexpr float kPickupSatKnee    = 0.80f;

inline constexpr float kDispersionDriveDepth = 0.25f;
inline constexpr float kRt60DriveDepth       = 0.40f;
inline constexpr float kRt60LevelDepth       = 0.25f;
inline constexpr float kMinRt60              = 0.60f;

// Post tone. A tone cut placed *inside* the feedback loop compounds on every
// pass and turns the tail into mud, so the level dependent darkening happens
// after the pickup, where it stays gentle and predictable.
inline constexpr float kPostToneDriveDepth = 0.30f;
inline constexpr float kPostToneLevelDepth = 0.25f;

// Envelope followers: a fast pair feeds the transducer compression, a slow pair
// feeds the damper and the impact scheduler.
inline constexpr float kFastAttackSeconds  = 0.002f;
inline constexpr float kFastReleaseSeconds = 0.120f;
inline constexpr float kSlowAttackSeconds  = 0.010f;
inline constexpr float kSlowReleaseSeconds = 0.400f;
inline constexpr float kSlowNormalise      = 0.30f; // amplitude read as "loud"

// Impact layer. Restrained: one coil per event, nothing above 10 events/s.
inline constexpr float kHitOnDb        = -12.0f;
inline constexpr float kHitOffDb       = -15.0f;
inline constexpr float kHitReleaseSec  = 0.400f;
inline constexpr float kHitRateBase    = 0.50f;  // events/s at the on threshold
inline constexpr float kHitRatePerDb   = 1.80f;
inline constexpr float kHitRateMax     = 10.0f;
inline constexpr float kHitAmpScale    = 0.0060f;
inline constexpr float kHitAmpMax      = 0.060f;
inline constexpr float kHitShortestMs  = 1.5f;
inline constexpr float kHitLongestMs   = 3.0f;
inline constexpr float kHitSecondChance = 0.15f;
inline constexpr float kHitSecondMinMs = 6.0f;
inline constexpr float kHitSecondMaxMs = 12.0f;
inline constexpr float kHitSecondGain  = 0.60f;

// 2x oversampler for the transducer: one halfband branch (16 taps) plus a pure
// 7 sample delay for the other branch. Kaiser beta 6, cutoff at the base rate's
// Nyquist, DC gain 2 across both phases so zero stuffing stays unity.
inline constexpr int kHalfbandTaps = 16;
inline constexpr float kHalfbandE0[kHalfbandTaps] =
{
    -0.000631211f,  0.003535622f, -0.010418012f,  0.023979373f,
    -0.048504701f,  0.093182964f, -0.189999923f,  0.628849072f,
     0.628849072f, -0.189999923f,  0.093182964f, -0.048504701f,
     0.023979373f, -0.010418012f,  0.003535622f, -0.000631211f
};

//==============================================================================
inline float softClip (float x) noexcept
{
    // Same curve ROOM uses: linear below the knee, tanh towards the ceiling.
    constexpr float knee = 0.70f;
    constexpr float ceiling = 0.99f;
    const float magnitude = std::abs (x);

    if (magnitude <= knee)
        return x;

    const float shaped = knee + (ceiling - knee) * std::tanh ((magnitude - knee) / (ceiling - knee));
    return x < 0.0f ? -shaped : shaped;
}

inline float saturate (float x, float knee) noexcept
{
    return knee * std::tanh (x / knee);
}

/** RBJ band pass, used to colour the impact bursts. */
struct BiquadBandPass
{
    void set (double sampleRate, double frequency, double q) noexcept
    {
        const double w0 = 2.0 * juce::MathConstants<double>::pi
                            * juce::jlimit (10.0, sampleRate * 0.45, frequency) / sampleRate;
        const double cosw = std::cos (w0);
        const double alpha = std::sin (w0) / (2.0 * juce::jmax (0.05, q));
        const double a0 = 1.0 + alpha;

        b0 = (float) (alpha / a0);
        b1 = 0.0f;
        b2 = (float) (-alpha / a0);
        a1 = (float) (-2.0 * cosw / a0);
        a2 = (float) ((1.0 - alpha) / a0);
    }

    inline float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void reset() noexcept { z1 = z2 = 0.0f; }

    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;
};

//==============================================================================
template <bool kTank>
class SpringCore
{
public:
    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;

        const int maxDelay = (int) std::ceil (0.08 * sampleRate) + 64;

        for (auto& coil : coils)
            coil.prepare (maxDelay);

        for (int i = 0; i < kSprings; ++i)
        {
            injectGain[(size_t) i] = (i % 2 == 0 ? 0.55f : -0.55f) * (0.8f + 0.2f * (float) i);
            // Both pickups hear the same coils, so they are strongly correlated;
            // the small lateral term only adds a hint of stereo width.
            const float common  = (i % 3 == 0 ? 1.0f : (i % 3 == 1 ? -0.75f : 0.55f));
            const float lateral = (i & 1) == 0 ? 0.22f : -0.22f;

            pickupL[(size_t) i] = 0.50f * (common + lateral);
            pickupR[(size_t) i] = 0.50f * (common - lateral);

            for (auto& state : dispersionState[(size_t) i])
                state = 0.0f;
        }

        for (auto* smoother : { &smoothDecay, &smoothSize, &smoothFreeze })
            smoother->reset (sampleRate, 0.05);

        if constexpr (kTank)
            smoothDrive.reset (sampleRate, 0.03);

        // Springs are thin at the bottom and honky in the upper mid.
        inputHighPassL.setCutoff (sampleRate, 280.0);
        inputHighPassR.setCutoff (sampleRate, 280.0);

        toneL.setLowPass (sampleRate, 5200.0, 0.7);
        toneR.setLowPass (sampleRate, 5400.0, 0.7);

        coilPeakL.setPeak (sampleRate, 1900.0, 1.1, 5.5);
        coilPeakR.setPeak (sampleRate, 2300.0, 1.1, 5.0);

        if constexpr (kTank)
        {
            fastAttack  = onePoleCoefficient (kFastAttackSeconds);
            fastRelease = onePoleCoefficient (kFastReleaseSeconds);
            slowAttack  = onePoleCoefficient (kSlowAttackSeconds);
            slowRelease = onePoleCoefficient (kSlowReleaseSeconds);
            dcPole      = (float) std::exp (-2.0 * juce::MathConstants<double>::pi * 20.0 / sampleRate);

            hitBand.set (sampleRate, 2600.0, 0.7);
            hitHighPass.setHighPass (sampleRate, 1500.0, 0.7);
        }

        reset();
        updateCoefficients();
    }

    void reset()
    {
        for (auto& coil : coils)
            coil.clear();

        for (auto& states : dispersionState)
            for (auto& state : states)
                state = 0.0f;

        inputHighPassL.clear();
        inputHighPassR.clear();
        toneL.reset();
        toneR.reset();
        coilPeakL.reset();
        coilPeakR.reset();
        counter = 0;

        if constexpr (kTank)
        {
            halfbandInput.fill (0.0f);
            halfbandEven.fill (0.0f);
            halfbandOdd.fill (0.0f);
            dcBlockX = dcBlockY = 0.0f;
            fastEnv = slowEnv = 0.0f;
            senseDc = 0.0f;
            levelDb = -140.0f;
            hitGate = false;
            hitReleaseSamples = 0.0f;
            hitCooldown = 0;
            hitRemaining = 0;
            hitLength = 0;
            hitCoil = 0;
            hitAmp = 0.0f;
            hitSign = 1.0f;
            secondHitDelay = 0;
            secondHitAmp = 0.0f;
            hitBand.reset();
            hitHighPass.reset();
            rng = Lcg { 0x51F3A2B7u };
        }
    }

    void setParams (const AlgorithmParams& newParams)
    {
        params = newParams;

        smoothDecay.setTargetValue (params.decay);
        smoothSize.setTargetValue (params.size);
        smoothFreeze.setTargetValue (params.freeze);

        if constexpr (kTank)
            smoothDrive.setTargetValue (juce::jlimit (0.0f, 1.0f, params.drive));
    }

    void snap()
    {
        for (auto* smoother : { &smoothDecay, &smoothSize, &smoothFreeze })
            smoother->setCurrentAndTargetValue (smoother->getTargetValue());

        if constexpr (kTank)
            smoothDrive.setCurrentAndTargetValue (smoothDrive.getTargetValue());

        updateCoefficients();
    }

    //==========================================================================
    void processSample (float inL, float inR, float& outL, float& outR)
    {
        if (++counter >= kChunk)
        {
            counter = 0;
            bool changed = advanceSmoothers (kChunk, { &smoothDecay, &smoothSize, &smoothFreeze });
            if constexpr (kTank)
            {
                advanceSmoothers (kChunk, { &smoothDrive });
                // The tank's working point also follows the input envelope.
                changed = true;
            }
            if (changed)
                updateCoefficients();
        }

        const float inputGain = 1.0f - freezeAmount;

        const float mono = 0.5f * (inL + inR);
        float excite = inputHighPassL.process (mono) * inputGain;

        float hitValue = 0.0f;
        int hitTarget = -1;

        if constexpr (kTank)
        {
            // The scheduler looks at what reaches the tank: the program input
            // after the Drive gain, DC removed. Measuring |excite| instead
            // would put the gate on the far side of the tank's own 280 Hz
            // shaping, which sits roughly 9 dB below the program level and made
            // the gate open far too late.
            const float senseInput = mono * inputGain;
            senseDc += (senseInput - senseDc) * (1.0f - dcPole);
            const float sense = std::abs (senseInput - senseDc) * driveGain;

            excite = driveStage (excite);

            // Two envelope followers: the fast one tracks the transducer, the
            // slow one tracks how hard the tank has been driven for a while.
            const float fastCoefficient = sense > fastEnv ? fastAttack : fastRelease;
            fastEnv += (sense - fastEnv) * fastCoefficient;

            const float slowCoefficient = sense > slowEnv ? slowAttack : slowRelease;
            slowEnv += (sense - slowEnv) * slowCoefficient;

            updateHitScheduler();

            if (hitRemaining > 0)
            {
                hitValue = nextHitSample();
                hitTarget = hitCoil;
            }
        }

        float spring[kSprings];

        for (int i = 0; i < kSprings; ++i)
        {
            float y = coils[(size_t) i].read (coilDelay[(size_t) i]);

            // Dispersion cascade inside the loop: this is the spring sound.
            auto& states = dispersionState[(size_t) i];

            for (int s = 0; s < kDispersion; ++s)
            {
                const float delayed = states[(size_t) s];
                const float v = y + dispersionCoefficient * delayed;
                states[(size_t) s] = v;
                y = delayed - dispersionCoefficient * v;
            }

            spring[i] = y;

            const float feedback = y * coilGain[(size_t) i];
            float injection = excite * injectGain[(size_t) i];

            if constexpr (kTank)
                if (i == hitTarget)
                    injection += hitValue;

            coils[(size_t) i].push (feedback + injection);

            // A little of the dispersed signal is picked up acoustically too.
            spring[i] += excite * injectGain[(size_t) i] * 0.15f;
        }

        float wetL = 0.0f, wetR = 0.0f;

        for (int i = 0; i < kSprings; ++i)
        {
            wetL += spring[i] * pickupL[(size_t) i];
            wetR += spring[i] * pickupR[(size_t) i];
        }

        if constexpr (kTank)
        {
            // The pickup is magnetic and it saturates: this is where a pushed
            // tank starts to compress and turn bright transients into blunt ones.
            wetL = saturate (wetL, kPickupSatKnee);
            wetR = saturate (wetR, kPickupSatKnee);
        }

        wetL = coilPeakL.process (toneL.process (wetL * 2.0f));
        wetR = coilPeakR.process (toneR.process (wetR * 2.0f));

        if constexpr (kTank)
        {
            outL = softClip (wetL * kOutputTrim);
            outR = softClip (wetR * kOutputTrim);
        }
        else
        {
            outL = juce::jlimit (-4.0f, 4.0f, wetL * kOutputTrim);
            outR = juce::jlimit (-4.0f, 4.0f, wetR * kOutputTrim);
        }
    }

private:
    //==========================================================================
    void updateCoefficients()
    {
        // 0.9 s (short tank) to 6 s (a long, loose tank with the feedback up).
        rt60 = 0.9f * std::pow (6.0f / 0.9f, juce::jlimit (0.0f, 1.0f, smoothDecay.getCurrentValue()));
        const float sizeScale = 0.78f + 0.50f * smoothSize.getCurrentValue();
        freezeAmount = juce::jlimit (0.0f, 1.0f, smoothFreeze.getCurrentValue());

        dispersionCoefficient = kDispersionGain;

        float tankRt60 = rt60;

        if constexpr (kTank)
        {
            drive = juce::jlimit (0.0f, 1.0f, smoothDrive.getCurrentValue());

            const float driveDb = (drive - 0.5f) * 2.0f * kDriveRangeDb;
            driveGain = std::pow (10.0f, driveDb / 20.0f);
            driveComp = std::pow (driveGain, -kDriveCompensation);

            // The knob's upper half is the overdrive range; below noon the tank
            // is only turned down, never reshaped.
            driveNorm = juce::jlimit (0.0f, 1.0f, (drive - 0.5f) * 2.0f);

            const float envSlow = juce::jlimit (0.0f, 1.0f, slowEnv / kSlowNormalise);

            // The working point of the tank moves with the drive setting and,
            // through the slow follower, with the level of the material. A tank
            // driven hard disperses more, damps faster and turns darker.
            dispersionCoefficient = kDispersionGain * (1.0f + kDispersionDriveDepth * driveNorm);

            tankRt60 = juce::jmax (kMinRt60,
                                   rt60 * (1.0f - kRt60DriveDepth * driveNorm)
                                        * (1.0f - kRt60LevelDepth * envSlow));

            const float toneScale = (1.0f - kPostToneDriveDepth * driveNorm)
                                      * (1.0f - kPostToneLevelDepth * envSlow);

            toneL.setLowPass (sampleRate, 5200.0 * (double) toneScale, 0.7);
            toneR.setLowPass (sampleRate, 5400.0 * (double) toneScale, 0.7);
        }

        for (int i = 0; i < kSprings; ++i)
        {
            const float delay = juce::jmax (32.0f, kCoilMs[i] * 0.001f * (float) sampleRate * sizeScale);
            coilDelay[(size_t) i] = delayForFreeze (delay, freezeAmount);

            // Springs lose their top end quickly; the low mid rings longest.
            const float g = loopGainForRt60 (delay, sampleRate, tankRt60 * 1.05f);
            coilGain[(size_t) i] = gainForFreeze (juce::jmin (0.9999f, g), freezeAmount);
        }
    }

    //==========================================================================
    // TANK: transducer. Two times oversampled so the harmonics that the drive
    // generates do not fold back as aliasing hash.
    float driveStage (float x) noexcept
    {
        for (int j = kHalfbandTaps - 1; j > 0; --j)
            halfbandInput[(size_t) j] = halfbandInput[(size_t) (j - 1)];

        halfbandInput[0] = x;

        float even = 0.0f;

        for (int j = 0; j < kHalfbandTaps; ++j)
            even += kHalfbandE0[j] * halfbandInput[(size_t) j];

        const float odd = halfbandInput[7];

        const float bias = kTransducerBias;
        const float biasTanh = std::tanh (bias);

        const float evenDrive = even * driveGain;
        const float oddDrive  = odd  * driveGain;

        auto transducer = [bias, biasTanh] (float value) noexcept
        {
            return kTransducerKnee * (std::tanh ((value / kTransducerKnee) + bias) - biasTanh);
        };

        const float te = transducer (evenDrive);
        const float to = transducer (oddDrive);

        for (int j = kHalfbandTaps - 1; j > 0; --j)
        {
            halfbandEven[(size_t) j] = halfbandEven[(size_t) (j - 1)];
            halfbandOdd[(size_t) j]  = halfbandOdd[(size_t) (j - 1)];
        }

        halfbandEven[0] = te;
        halfbandOdd[0] = to;

        float z = 0.0f;

        for (int j = 0; j < kHalfbandTaps; ++j)
            z += kHalfbandE0[j] * halfbandEven[(size_t) j];

        z = 0.5f * (z + halfbandOdd[8]);

        const float dc = z - dcBlockX + dcPole * dcBlockY;
        dcBlockX = z;
        dcBlockY = dc;

        return dc * driveComp;
    }

    //==========================================================================
    float onePoleCoefficient (float seconds) const noexcept
    {
        return (float) (1.0 - std::exp (-1.0 / (juce::jmax (1.0e-6, (double) seconds) * sampleRate)));
    }

    void updateHitScheduler() noexcept
    {
        levelDb = 20.0f * std::log10 (juce::jmax (1.0e-7f, fastEnv));

        if (freezeAmount > 0.5f)
        {
            hitGate = false;
            hitRemaining = 0;
            secondHitDelay = 0;
            hitCooldown = 0;
            return;
        }

        if (levelDb > kHitOnDb)
        {
            hitGate = true;
            hitReleaseSamples = kHitReleaseSec * (float) sampleRate;
        }
        else if (hitGate && levelDb < kHitOffDb)
        {
            hitReleaseSamples -= 1.0f;

            if (hitReleaseSamples <= 0.0f)
                hitGate = false;
        }

        if (hitCooldown > 0)
            --hitCooldown;

        if (secondHitDelay > 0 && --secondHitDelay == 0 && hitRemaining <= 0)
            startHit (secondHitAmp);

        if (hitGate && hitCooldown == 0 && hitRemaining <= 0)
        {
            const float level = juce::jmax (0.0f, levelDb - kHitOnDb);
            const float amplitude = juce::jmin (kHitAmpMax, kHitAmpScale * std::pow (level, 1.5f));

            startHit (amplitude);

            const float rate = juce::jlimit (0.1f, kHitRateMax,
                                             kHitRateBase * std::pow (kHitRatePerDb, level));
            hitCooldown = (int) (sampleRate / rate);
        }
    }

    void startHit (float amplitude) noexcept
    {
        if (amplitude <= 0.0001f)
            return;

        const float shortest = kHitShortestMs * 0.001f * (float) sampleRate;
        const float longest  = kHitLongestMs * 0.001f * (float) sampleRate;

        hitLength = (int) (shortest + (longest - shortest) * rng.next());
        hitRemaining = hitLength;
        hitCoil = juce::jlimit (0, kSprings - 1, (int) (rng.next() * (float) kSprings));
        hitAmp = amplitude;
        hitSign = rng.signedUnit();

        hitBand.reset();
        hitHighPass.reset();

        if (rng.next() < kHitSecondChance && secondHitDelay == 0)
        {
            const float low  = kHitSecondMinMs * 0.001f * (float) sampleRate;
            const float high = kHitSecondMaxMs * 0.001f * (float) sampleRate;
            secondHitDelay = (int) (low + (high - low) * rng.next());
            secondHitAmp = amplitude * kHitSecondGain;
        }
    }

    float nextHitSample() noexcept
    {
        const int position = hitLength - hitRemaining;
        --hitRemaining;

        const float window = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi
                                                       * (float) (position + 1) / (float) (hitLength + 1));
        const float noise = rng.next() * 2.0f - 1.0f;
        const float shaped = hitHighPass.process (hitBand.process (noise));

        return shaped * window * hitAmp * hitSign;
    }

    //==========================================================================
    double sampleRate = 48000.0;
    AlgorithmParams params;
    int counter = 0;

    std::array<DelayLine, kSprings> coils;
    std::array<std::array<float, kDispersion>, kSprings> dispersionState {};
    std::array<float, kSprings> coilDelay {};
    std::array<float, kSprings> coilGain {};
    std::array<float, kSprings> injectGain {};
    std::array<float, kSprings> pickupL {}, pickupR {};

    OnePole inputHighPassL, inputHighPassR;
    Biquad toneL, toneR;
    Biquad coilPeakL, coilPeakR;

    juce::SmoothedValue<float> smoothDecay, smoothSize, smoothFreeze;
    float rt60 = 2.2f;
    float freezeAmount = 0.0f;
    float dispersionCoefficient = kDispersionGain;

    // ---- TANK only --------------------------------------------------------
    juce::SmoothedValue<float> smoothDrive;
    float drive = 0.5f;
    float driveNorm = 0.0f;
    float driveGain = 1.0f;
    float driveComp = 1.0f;

    std::array<float, kHalfbandTaps> halfbandInput {};
    std::array<float, kHalfbandTaps> halfbandEven {};
    std::array<float, kHalfbandTaps> halfbandOdd {};
    float dcBlockX = 0.0f, dcBlockY = 0.0f;
    float dcPole = 0.9973f;

    float fastEnv = 0.0f, slowEnv = 0.0f;
    float senseDc = 0.0f;
    float fastAttack = 0.1f, fastRelease = 0.001f;
    float slowAttack = 0.01f, slowRelease = 0.0005f;
    float levelDb = -140.0f;

    Lcg rng { 0x51F3A2B7u };
    bool hitGate = false;
    float hitReleaseSamples = 0.0f;
    int hitCooldown = 0;
    int hitRemaining = 0, hitLength = 0, hitCoil = 0;
    float hitAmp = 0.0f, hitSign = 1.0f;
    int secondHitDelay = 0;
    float secondHitAmp = 0.0f;
    BiquadBandPass hitBand;
    Biquad hitHighPass;
};

} // namespace spring
} // namespace rivage
