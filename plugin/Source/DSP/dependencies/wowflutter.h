// WowFlutter (WF_OnePoleLP, WowFlutterOscillator): Wow/flutter control-signal generator (1 Hz wow, 4/12 Hz flutter,
//   Ornstein-Uhlenbeck drift) for tape emulation.
// Realtime-safety: No allocation/locks in process(); uses PCG32 for stateless LCG-based noise.

// wowflutter.h
//
// Enhanced wow/flutter model with dynamic rate scaling and organic drift.
// Classic tuning preserved at Condition=0.
//
// Design: Wow/flutter model = 1 Hz wow, 4 Hz & 12 Hz flutter + filtered noise,
// tuned for classic tape-echo motion. Frequencies and relative amplitudes are part of the sound.
// This file is a wow and flutter control signal generator.
//
// Features:
// - Dynamic rate scaling based on delay time (tape speed)
// - Ornstein-Uhlenbeck drift for organic random motion
// - Scrape flutter for high-frequency tape-head friction artifacts
// - Stereo phase decorrelation support

#pragma once

#include <juce_core/juce_core.h>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include "RbjBiquad.h"
#include "../PCG32.h"

// Every noise source in this file is per-sample i.i.d. white feeding a
// fixed-Hz filter, so its filtered RMS is proportional to 1/sqrt(fs): an
// unscaled generator is ~3 dB quieter at 96 kHz and ~6 dB quieter at 192 kHz
// than at 48 kHz, the rate the levels are tuned at. Scale the generator
// amplitude to hold the level. Exactly 1.0 at 48 kHz.
inline float wfWhiteNoiseRateScale(float sampleRate) noexcept
{
    return (sampleRate > 0.0f) ? std::sqrt(sampleRate / 48000.0f) : 1.0f;
}

// Poisson-style per-sample event probabilities describe a rate in events per
// second, so they must shrink as the sample rate rises or the event rate
// scales with fs. Exactly 1.0 at 48 kHz.
inline float wfEventRateScale(float sampleRate) noexcept
{
    return (sampleRate > 0.0f) ? (48000.0f / sampleRate) : 1.0f;
}

class WF_OnePoleLP
{
public:
    WF_OnePoleLP() = default;

    void reset() { z1 = 0.0f; }

    void setState(float value) { z1 = value; }

    void setCutoff(float fc, float sampleRate)
    {
        float w = 2.0f * juce::MathConstants<float>::pi * fc / sampleRate;
        alpha = w / (w + 1.0f);
    }

    float process(float x)
    {
        z1 = z1 + alpha * (x - z1);
        return z1;
    }

private:
    float alpha = 0.1f;
    float z1 = 0.0f;
};

class OrnsteinUhlenbeckDrift
{
public:
    OrnsteinUhlenbeckDrift() { rng.seedFromRandomDevice(); }

    void seed(uint32_t seed) { rng.seed(static_cast<uint64_t>(seed) ^ 0xC2B2AE3D27D4EB4FULL, 0xA5A5A5A5A5A5A5A5ULL); }

    void prepare(float sampleRate)
    {
        sqrtDelta = 1.0f / std::sqrt(sampleRate);
        T = 1.0f / sampleRate;
        // Start ON the mean, not at 1.0. The comment claimed "start at mean"
        // but `mean` is setAmount()'s shaped amount (~0.04 at the default), so
        // the process started ~0.96 above its mean and process() returned
        // lpf(y) - mean, i.e. a ~+1.0 excursion decaying over ~1 s. Scaled by
        // 0.5 * 0.35 * Depth that is a ~0.17 ms delay-time glide, so every
        // prepareToPlay produced an audible pitch settle on the first second of
        // tape. Priming y and the smoothing filter to the mean makes the drift
        // output start at exactly 0.
        y = mean;

        // Low-pass filter at 10Hz for slow drift
        lpf.setCutoff(10.0f, sampleRate);
        lpf.reset();
        lpf.setState(mean);
    }

    /**
     * Set drift parameters
     * @param amt - Drift amount (0-1), affects noise magnitude and damping
     */
    void setAmount(float amount)
    {
        // Scale drift parameters from normalized amount
        float scaledAmt = std::pow(amount, 1.25f);
        this->amt = scaledAmt;
        damping = scaledAmt * 20.0f + 1.0f;  // More amt = faster return to mean
        mean = scaledAmt;
    }

    /**
     * Process one sample of drift
     * @return Drift value (centered around 0, scaled by amount)
     */
    float process()
    {
        // Generate noise sample
        float noise = rng.nextSignedFloat();

        // Ornstein-Uhlenbeck update: y += sqrt(dt)*noise*amt + damping*(mean-y)*dt
        y += sqrtDelta * noise * amt;
        y += damping * (mean - y) * T;

        // Low-pass filter for smooth drift
        return lpf.process(y) - mean;  // Return deviation from mean
    }

private:
    PCG32 rng;

    float sqrtDelta = 0.0f;
    float T = 0.0f;
    float y = 1.0f;

    float amt = 0.0f;
    float mean = 0.0f;
    float damping = 1.0f;

    WF_OnePoleLP lpf;
};

class SineOsc
{
public:
    SineOsc() = default;
    ~SineOsc() = default;

    void reset(float sampleRate)
    {
        SampleRate = sampleRate;
        phase = 0.0f;
        // Keep the configured frequency and re-derive the increment for the new
        // rate. Zeroing phaseInc here would freeze every wow/flutter oscillator
        // at DC until the next setParams() call, and
        // WowFlutterSignalModel::init() deliberately calls updateParameters()
        // *before* reset(), so the increments configured there would be lost.
        phaseInc = (SampleRate > 0.0f) ? (2.0f * juce::MathConstants<float>::pi * Frequency / SampleRate) : 0.0f;
    }

    void setPhase(float newPhase)
    {
        phase = newPhase;
        // Wrap to [0, 2π)
        while (phase >= 2.0f * juce::MathConstants<float>::pi)
            phase -= 2.0f * juce::MathConstants<float>::pi;
        while (phase < 0.0f)
            phase += 2.0f * juce::MathConstants<float>::pi;
    }

    void setParams(float amplitude, float frequency)
    {
        Frequency = frequency;
        Amplitude = amplitude;
        phaseInc = 2.0f * juce::MathConstants<float>::pi * Frequency / SampleRate;
    }

    float returnCurrentValue()
    {
        float val = Amplitude * std::sin(phase);
        phase += phaseInc;

        // Keep phase bounded to avoid precision loss
        if (phase >= 2.0f * juce::MathConstants<float>::pi)
            phase -= 2.0f * juce::MathConstants<float>::pi;

        return val;
    }

private:
    float SampleRate = 44100.0f;
    float phase = 0.0f;
    float phaseInc = 0.0f;
    float Frequency = 0.0f;
    float Amplitude = 0.0f;
};

class WhiteNoiseOsc
{
public:
    WhiteNoiseOsc() { rng.seedFromRandomDevice(); }
    ~WhiteNoiseOsc() = default;

    void seed(uint32_t seed) { rng.seed(static_cast<uint64_t>(seed) ^ 0xD1B54A32D192ED03ULL, 0x9E3779B97F4A7C15ULL); }

    void setParams(float amplitude) { Amplitude = amplitude; }

    float returnCurrentValue() { return Amplitude * rng.nextSignedFloat(); }

private:
    PCG32 rng;
    float Amplitude = 0.0f;
};

class ScrapeFlutterGenerator
{
public:
    ScrapeFlutterGenerator() { rng.seedFromRandomDevice(); }

    void seed(uint32_t seed) { rng.seed(static_cast<uint64_t>(seed) ^ 0x94D049BB133111EBULL, 0x165667B19E3779F9ULL); }

    void reset(float sampleRate)
    {
        SampleRate = sampleRate;
        noiseRateScale = wfWhiteNoiseRateScale(SampleRate);
        eventRateScale = wfEventRateScale(SampleRate);

        // Initialize multiple inharmonic oscillators for realistic scrape
        // Frequencies based on research: 500Hz - 3kHz primary range
        initializeOscillators();

        // Initialize bandpass filter for noise component (200Hz - 5kHz)
        initializeBandpassFilter();
    }

    /**
     * Initialize oscillators with inharmonic frequencies
     * Research shows scrape flutter has quasi-random frequency content
     */
    void initializeOscillators()
    {
        // Primary oscillators in the scrape flutter range
        // Using inharmonic ratios for realistic texture
        oscFreq[0] = 350.0f;   // Low scrape component
        oscFreq[1] = 780.0f;   // Near tape resonance (675-860Hz)
        oscFreq[2] = 1450.0f;  // Upper mid component
        oscFreq[3] = 2800.0f;  // High scrape component (2-5kHz range)

        // Randomize initial phases
        for (int i = 0; i < 4; ++i)
        {
            oscPhase[i] = rng.nextFloat();
            oscAmp[i] = 0.25f;  // Equal initial weights
        }

        // Frequency drift targets (slow random walk)
        for (int i = 0; i < 4; ++i)
        {
            freqDriftTarget[i] = oscFreq[i];
            freqDriftRate[i] = 0.0001f;  // Very slow drift
        }
    }

    /**
     * Initialize bandpass filter for scrape noise (200Hz - 5kHz)
     */
    void initializeBandpassFilter()
    {
        // High-pass at 200Hz
        float hpFc = 200.0f / SampleRate;
        hpAlpha = 1.0f - std::exp(-2.0f * juce::MathConstants<float>::pi * hpFc);
        hpState = 0.0f;

        // Low-pass at 5kHz
        float lpFc = 5000.0f / SampleRate;
        lpAlpha = 1.0f - std::exp(-2.0f * juce::MathConstants<float>::pi * lpFc);
        lpState = 0.0f;
    }

    /**
     * Set scrape flutter parameters
     * @param depth Overall scrape flutter intensity (0-1)
     * @param condition Tape wear level (0=pristine, 1=worn)
     */
    void setParameters(float depth, float condition)
    {
        Depth = depth;
        Condition = condition;

        // Calculate effective scrape intensity
        // Research: scrape flutter starts appearing at moderate wear
        float conditionScale = getScrapeIntensity(condition);
        EffectiveDepth = depth * conditionScale;

        // Modulation depth in samples (research: very small, 0.1-2 samples)
        // Higher condition = slightly more modulation
        ModDepthSamples = 0.1f + 1.5f * EffectiveDepth;

        // Update oscillator amplitudes based on condition
        // More worn = more energy in higher frequencies
        oscAmp[0] = 0.35f - 0.1f * condition;   // Less low freq when worn
        oscAmp[1] = 0.30f;                      // Resonance stays constant
        oscAmp[2] = 0.20f + 0.1f * condition;   // More upper mid when worn
        oscAmp[3] = 0.15f + 0.15f * condition;  // More high freq when worn

        // Noise amount increases with wear
        NoiseAmount = 0.2f + 0.4f * condition;
    }

    /**
     * Returns the delay modulation value only. No nonlinearity is applied to
     * audio at the base rate; intermodulation, if wanted, belongs inside an
     * oversampled stage.
     */
    float processDelayMod()
    {
        updateFrequencyDrift();
        float scrapeSignal = generateScrapeModulation();
        return scrapeSignal * ModDepthSamples * EffectiveDepth;
    }

private:
    /**
     * Calculate scrape intensity based on tape condition
     * Research: scrape flutter increases non-linearly with wear
     */
    float getScrapeIntensity(float condition)
    {
        // Scrape flutter starts appearing around 0.2 condition. An onset at
        // 0.10 measures only +0.08 dB in the scrape band, which is inaudible;
        // per-flavor scrape lives in presetScrapeScale instead.
        if (condition < 0.15f)
            return 0.0f;
        return std::pow((condition - 0.15f) / 0.85f, 1.3f);
    }

    /**
     * Update oscillator frequencies with slow random drift
     * Simulates the quasi-random nature of stick-slip friction
     */
    void updateFrequencyDrift()
    {
        for (int i = 0; i < 4; ++i)
        {
            // Slowly drift toward target
            oscFreq[i] += (freqDriftTarget[i] - oscFreq[i]) * freqDriftRate[i];

            // Occasionally pick a new drift target. The literal is a per-sample
            // probability tuned at 48 kHz, so scale it to keep the retarget
            // rate constant in events per second across sample rates.
            if (rng.nextFloat() < 0.0001f * eventRateScale)
            {
                // New target within ±20% of base frequency
                float baseFreq = getBaseFreq(i);
                float variation = 0.2f + 0.3f * Condition;  // More variation when worn
                freqDriftTarget[i] = baseFreq * (1.0f + variation * (rng.nextFloat() * 2.0f - 1.0f));
            }
        }

        // The drift pattern above depends on this generator's exact sequence,
        // which includes one more draw per sample here and a second on a
        // 0.02%-per-sample event. Those drawn values are not otherwise used.
        if (rng.nextFloat() < 0.0002f * eventRateScale)
            rng.nextFloat();
    }

    float getBaseFreq(int idx)
    {
        const float baseFreqs[4] = {350.0f, 780.0f, 1450.0f, 2800.0f};
        return baseFreqs[idx];
    }

    /**
     * Generate the composite scrape flutter modulation signal
     */
    float generateScrapeModulation()
    {
        float sum = 0.0f;

        // Sum inharmonic oscillators
        for (int i = 0; i < 4; ++i)
        {
            oscPhase[i] += oscFreq[i] / SampleRate;
            if (oscPhase[i] >= 1.0f)
                oscPhase[i] -= 1.0f;

            sum += oscAmp[i] * std::sin(oscPhase[i] * 2.0f * juce::MathConstants<float>::pi);
        }

        // Add bandpass-filtered noise for texture (level held across rates)
        float noise = rng.nextSignedFloat() * noiseRateScale;

        // High-pass filter
        float hp = noise - hpState;
        hpState += hpAlpha * (noise - hpState);

        // Low-pass filter
        lpState += lpAlpha * (hp - lpState);

        // Mix oscillators with filtered noise
        float output = (1.0f - NoiseAmount) * sum + NoiseAmount * lpState * 2.0f;

        return output;
    }

    // Random number generation
    PCG32 rng;

    float SampleRate = 44100.0f;
    float Depth = 0.0f;
    float Condition = 0.0f;
    float EffectiveDepth = 0.0f;
    float ModDepthSamples = 0.5f;
    float NoiseAmount = 0.3f;

    // Inharmonic oscillators (research: multiple frequencies in 350Hz-3kHz range)
    float oscPhase[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float oscFreq[4] = {350.0f, 780.0f, 1450.0f, 2800.0f};
    float oscAmp[4] = {0.25f, 0.25f, 0.25f, 0.25f};

    // Frequency drift state
    float freqDriftTarget[4] = {350.0f, 780.0f, 1450.0f, 2800.0f};
    float freqDriftRate[4] = {0.0001f, 0.0001f, 0.0001f, 0.0001f};

    // Bandpass filter state
    float hpState = 0.0f;
    float lpState = 0.0f;
    float hpAlpha = 0.1f;
    float lpAlpha = 0.5f;

    // Sample-rate normalizers (1.0 at 48 kHz)
    float noiseRateScale = 1.0f;
    float eventRateScale = 1.0f;
};

class WowFlutterSignalModel
{
public:
    WowFlutterSignalModel() = default;
    ~WowFlutterSignalModel() = default;

    void setDeterministicSeed(uint32_t seed)
    {
        deterministicSeedEnabled = true;
        deterministicSeed = seed;
    }

    void init()
    {
        // Default classic values
        updateParameters(1.0f, 1.0f, 1.0f, 0.0f, 175.0f);
    }

    void reset(float sampleRate)
    {
        SampleRate = sampleRate;

        if (deterministicSeedEnabled)
        {
            driftProc.seed(deterministicSeed ^ 0x9E3779B9u);
            noise.seed(deterministicSeed ^ 0xBB67AE85u);
            scrapeFlutter.seed(deterministicSeed ^ 0x3C6EF372u);
        }

        // Reset periodic oscillators
        wowOsc.reset(sampleRate);
        flutter1.reset(sampleRate);
        flutter2.reset(sampleRate);

        // Reset noise filter
        noiseFilt.Init(BiquadType::LPF, 1.0f, sampleRate, 0.707f, 0.0f);
        noiseFilt.Reset(sampleRate);
        // Re-apply the rate-normalized amplitude: init() runs updateParameters()
        // before reset(), so the amplitude set there used the stale rate.
        noise.setParams(8.0f * wfWhiteNoiseRateScale(sampleRate));

        // Reset drift processor (Ornstein-Uhlenbeck)
        driftProc.prepare(sampleRate);

        // Reset scrape flutter
        scrapeFlutter.reset(sampleRate);
        stereoPhaseOffsetApplied = false;
    }

    /**
     * Set stereo phase offset for decorrelation
     * @param offsetRadians - Phase offset in radians (0 = no offset)
     * Typical: 5-15% of 2π for subtle stereo width
     */
    void setStereoPhaseOffset(float offsetRadians)
    {
        if (stereoPhaseOffsetApplied && std::abs(StereoPhaseOffset - offsetRadians) <= 1.0e-6f)
            return;

        StereoPhaseOffset = offsetRadians;
        stereoPhaseOffsetApplied = true;

        // Apply to oscillators
        wowOsc.setPhase(offsetRadians * 0.3f);    // Subtle wow offset
        flutter1.setPhase(offsetRadians * 0.6f);  // Medium flutter offset
        flutter2.setPhase(offsetRadians * 0.9f);  // Larger flutter offset
    }

    /**
     * Update wow/flutter parameters with dynamic rate scaling
     * Design: TapeCondition scales modulation depth (worn tape = more wobble).
     * Condition = 0 reproduces the plain Depth behavior with no wear scaling.
     *
     * @param depth - Overall modulation depth (0=none, 1=classic, up to 4=extreme)
     * @param wowRate - Rate multiplier for wow component (0.25-4x)
     * @param flutterRate - Rate multiplier for flutter components (0.25-4x)
     * @param tapeCondition - Tape age: 0=pristine, 1=worn (scales depth + drift + scrape)
     * @param delayTimeMs - Current delay time in ms (for dynamic rate scaling)
     */
    void updateParameters(float depth, float wowRate, float flutterRate, float tapeCondition,
                          float delayTimeMs = 175.0f, float scrapeDepthScale = 1.0f)
    {
        Depth = depth;
        WowRate = wowRate;
        FlutterRate = flutterRate;
        TapeCondition = tapeCondition;
        DelayTimeMs = delayTimeMs;

        // Condition adds bounded mechanical wear while keeping Depth as the
        // main wobble-depth authority. Pristine tape preserves the baseline.
        const float conditionShaped = std::pow(std::clamp(TapeCondition, 0.0f, 1.0f), 0.85f);
        ConditionScale = 1.0f + 0.12f * conditionShaped;

        // ================================================================
        // Dynamic Rate Scaling based on delay time
        // Shorter delay (faster tape) = higher modulation frequencies
        // Classic range: 175ms (slowest) -> 70ms (fastest). Tempo-synced
        // transports can be longer, so the same inverse-speed relationship
        // continues below 1x with a practical floor for musical motion.
        // ================================================================
        float clampedDelay = std::clamp(delayTimeMs, 70.0f, 6000.0f);
        // speedFactor = 175 / delayTimeMs (inversely proportional to delay)
        float speedFactor = std::clamp(175.0f / clampedDelay, 0.125f, 2.5f);

        // Design: Base frequencies and amplitudes (classic tuning at Condition=0).
        // Wow: ~0.5-1.5Hz based on speed, amplitude 1.0
        // Flutter mid: ~2-6Hz based on speed, amplitude 0.5
        // Flutter fast: ~6-18Hz based on speed, amplitude 0.2
        float wowBaseFreq = 1.0f * speedFactor;
        float flutter1BaseFreq = 4.0f * speedFactor;
        float flutter2BaseFreq = 12.0f * speedFactor;

        // Apply user rate multipliers
        wowOsc.setParams(1.0f, wowBaseFreq * WowRate);
        flutter1.setParams(0.5f, flutter1BaseFreq * FlutterRate);
        flutter2.setParams(0.2f, flutter2BaseFreq * FlutterRate);

        // Noise component (filtered to ~1Hz for slow random). The amplitude is
        // rate-normalized: 8.0 into a fixed 1 Hz filter yields ~3 dB less
        // irregularity per doubling of the sample rate without it.
        noise.setParams(8.0f * wfWhiteNoiseRateScale(SampleRate));

        // ================================================================
        // Random Drift (Ornstein-Uhlenbeck process)
        // Worn tape gets a little more random transport drift without
        // overpowering the primary wow/flutter depth control.
        // ================================================================
        float driftAmount = 0.08f + 0.05f * conditionShaped;
        driftProc.setAmount(driftAmount * Depth);

        // ================================================================
        // Scrape Flutter (high-frequency friction artifacts)
        // Very small modulation depth (sub-sample to 2 samples)
        // ================================================================
        // Scrape intensity is the high-frequency wear texture; keep it below
        // the low-rate wow/flutter components and shape it mostly by condition.
        float scrapeDepth = Depth * (0.16f + 0.24f * conditionShaped) * scrapeDepthScale;
        scrapeDepth = std::clamp(scrapeDepth, 0.0f, 0.65f);
        scrapeFlutter.setParameters(scrapeDepth, conditionShaped);
    }

    /**
     * Process one sample
     * @return Wow/flutter modulation value in milliseconds
     */
    float processSample()
    {
        // Sum periodic oscillators (design-tuned amplitudes)
        float periodic = wowOsc.returnCurrentValue() + flutter1.returnCurrentValue() + flutter2.returnCurrentValue();

        // Add filtered noise for irregularity
        float filteredNoise = noiseFilt.ProcessSample(noise.returnCurrentValue());
        periodic += filteredNoise;

        // Add Ornstein-Uhlenbeck drift (scaled by depth)
        float drift = driftProc.process() * 0.5f;  // Scale drift contribution

        // Add scrape flutter (very subtle high-frequency component)
        // Uses processDelayMod() since we only need delay modulation here
        float scrape = scrapeFlutter.processDelayMod();

        // Combine all components
        float combined = periodic + drift + scrape;

        // Design: Apply depth scaling. At Depth=1 and Condition=0, output matches baseline.
        // The 0.35f factor sets the overall modulation amplitude; do not change arbitrarily.
        return combined * 0.35f * Depth * ConditionScale;
    }

private:
    // Periodic oscillators
    SineOsc wowOsc;    // ~0.5-1.5Hz wow
    SineOsc flutter1;  // ~2-6Hz mid flutter
    SineOsc flutter2;  // ~6-18Hz fast flutter

    // Noise component
    WhiteNoiseOsc noise;
    RbjBiquad noiseFilt;

    // Ornstein-Uhlenbeck drift (organic random motion)
    OrnsteinUhlenbeckDrift driftProc;

    // Scrape flutter (high-frequency tape-head friction)
    ScrapeFlutterGenerator scrapeFlutter;

    // Parameters
    float SampleRate = 44100.0f;
    float Depth = 1.0f;              // 0-4, 1=classic
    float WowRate = 1.0f;            // 0.25-4x
    float FlutterRate = 1.0f;        // 0.25-4x
    float TapeCondition = 0.0f;      // 0=pristine, 1=worn
    float ConditionScale = 1.0f;     // small worn-tape lift; Depth remains the main modulation authority
    float DelayTimeMs = 175.0f;      // Current delay time for rate scaling
    float StereoPhaseOffset = 0.0f;  // Stereo decorrelation offset
    bool stereoPhaseOffsetApplied = false;

    bool deterministicSeedEnabled = false;
    uint32_t deterministicSeed = 0;
};
