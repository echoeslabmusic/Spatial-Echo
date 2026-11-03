// PreampProcessor: Transistor-style input stage with ADAA saturation and EQ.
// Realtime-safety: No allocation in process(); oversampling pre-configured at Reset()

#pragma once

#include <cmath>
#include <vector>
#include <algorithm>
#include <array>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "dependencies/IIROversampler.h"

/**
 * PreampProcessor - transistor-style input stage model
 *
 * Design: Always-on input stage with asymmetrical saturation, compression, and EQ.
 *
 * Implementation based on standard audio DSP techniques:
 * - First-order ADAA for antialiased saturation (Parker et al., DAFx 2019)
 * - Asymmetric soft-clipping for BJT transistor character
 * - TDF-II biquad filters for EQ (textbook digital filter design)
 * - One-pole lowpass for input loading simulation
 * - Envelope-based dynamic compression
 *
 * Features:
 * - Asymmetric BJT saturation with first-order ADAA (reduces aliasing)
 * - Proper TDF-II biquad filters (HPF and Mid Bell)
 * - Input loading simulation (RC lowpass)
 * - Dynamic compression (envelope following)
 * - Coupling capacitor DC blocking
 * - "Warm, full-bodied tone with mellow mids and smooth highs"
 *
 * Position in signal flow: after dry capture, before tape/reverb processing.
 * The transparent dry path bypasses this stage; wet/effects input passes through it.
 */
class PreampProcessor
{
public:
    PreampProcessor() = default;
    ~PreampProcessor() = default;

    // Oversampling is always on in the plug-in. Turning it off selects the
    // base-rate saturation path, which the alias A/B tests use as a control.
    void setOversamplingEnabled(bool enabled)
    {
        oversampleEnabled = enabled;

        if (oversampleEnabled)
        {
            satOversampler.prepare(NumChannels, MaxBlockSize, kOversampleFactor);
            satOversampler.reset();
        }

        if (SampleRate > 0.0f)
            updateFilters();
    }

    void reset(float sampleRate, int numChannels, int maxBlockSize)
    {
        SampleRate = sampleRate;
        NumChannels = numChannels;
        MaxBlockSize = std::max(1, maxBlockSize);

        // Resize all per-channel state vectors
        inputCouplingState.resize(static_cast<size_t>(numChannels), 0.0f);
        inputLoadingState.resize(static_cast<size_t>(numChannels), 0.0f);
        preSatLPFState.resize(static_cast<size_t>(numChannels), 0.0f);
        postSatLPFState.resize(static_cast<size_t>(numChannels), 0.0f);
        postSatLPFState2.resize(static_cast<size_t>(numChannels), 0.0f);
        postSatDCState.resize(static_cast<size_t>(numChannels), 0.0f);
        compressionEnvelope.resize(static_cast<size_t>(numChannels), 0.0f);
        outputCouplingState.resize(static_cast<size_t>(numChannels), 0.0f);

        // ADAA state vectors
        adaaX1.resize(static_cast<size_t>(numChannels), 0.0f);

        // TDF-II biquad states (2 state vars per filter per channel)
        hpfZ1.resize(static_cast<size_t>(numChannels), 0.0f);
        hpfZ2.resize(static_cast<size_t>(numChannels), 0.0f);
        midZ1.resize(static_cast<size_t>(numChannels), 0.0f);
        midZ2.resize(static_cast<size_t>(numChannels), 0.0f);

        // Initialize parameter smoothers (~5ms ramp time, sample-rate aware)
        smoothedMakeupGain.reset(sampleRate, smoothingTimeSeconds);
        smoothedMakeupGain.setCurrentAndTargetValue(targetMakeupGain);
        smoothedInputLevel.reset(sampleRate, smoothingTimeSeconds);
        smoothedInputLevel.setCurrentAndTargetValue(targetInputLevel);
        smoothedAsymmetry.reset(sampleRate, smoothingTimeSeconds);
        smoothedAsymmetry.setCurrentAndTargetValue(targetAsymmetry);
        smoothedWarmth.reset(sampleRate, smoothingTimeSeconds);
        smoothedWarmth.setCurrentAndTargetValue(targetWarmth);

        // Calculate all filter coefficients
        updateFilters();

        // Oversampler + buffers for the 4x saturation stage
        preSatBuffer.setSize(numChannels, MaxBlockSize, false, false, true);
        sampleAsymmetry.resize(static_cast<size_t>(MaxBlockSize), 0.0f);
        sampleWarmth.resize(static_cast<size_t>(MaxBlockSize), 0.0f);
        sampleMakeupGain.resize(static_cast<size_t>(MaxBlockSize), 1.0f);

        if (oversampleEnabled)
        {
            satOversampler.prepare(numChannels, MaxBlockSize, kOversampleFactor);
            satOversampler.reset();
        }
    }

    // Auto-panic flush: zero all recirculating filter/envelope state in place
    // (no reallocation - audio-thread safe). Coefficients and smoothed targets
    // are untouched; only sample history that could hold a stuck value goes.
    void clearAudioState()
    {
        auto zero = [](std::vector<float>& v) { std::fill(v.begin(), v.end(), 0.0f); };
        zero(inputCouplingState);
        zero(inputLoadingState);
        zero(preSatLPFState);
        zero(postSatLPFState);
        zero(postSatLPFState2);
        zero(postSatDCState);
        zero(compressionEnvelope);
        zero(outputCouplingState);
        zero(adaaX1);
        zero(hpfZ1);
        zero(hpfZ2);
        zero(midZ1);
        zero(midZ2);
        if (oversampleEnabled)
            satOversampler.reset();
    }

    /**
     * Update preamp parameters
     * Design: The preamp is always active. The Input Level knob controls signal level
     * into the preamp (1.0 = unity, higher values increase saturation).
     *
     * @param inputLevel - Linear input gain (0-4 range, 1.0 = unity)
     */
    void updateParameters(float inputLevel)
    {
        // Clamp input to valid range and avoid log(0) / invalid smoothing targets
        inputLevel = juce::jlimit(0.0f, maxInputGain, inputLevel);
        const float safeInputLevel = std::max(inputLevel, inputLevelEpsilon);

        // Design: Preamp always active
        Enabled = true;

        // Set target input level (smoothers will interpolate)
        targetInputLevel = safeInputLevel;
        smoothedInputLevel.setTargetValue(targetInputLevel);

        // Design: Drive is derived from input level (linear -> dB)
        float driveDb = juce::Decibels::gainToDecibels(safeInputLevel);
        DriveDb = driveDb;

        // No auto compensation: input level is user-controlled.
        targetMakeupGain = 1.0f;
        smoothedMakeupGain.setTargetValue(targetMakeupGain);

        // Adjust asymmetry based on input level for classic BJT behavior
        // More input = more asymmetric clipping = more even harmonics
        float driveNorm = juce::jlimit(0.0f, maxDriveDb, driveDb) / maxDriveDb;
        targetAsymmetry = 0.015f + 0.03f * driveNorm;  // Trace DC bias; tape remains odd-harmonic dominant
        targetWarmth = 0.08f + 0.07f * driveNorm;      // 2nd harmonic injection
        smoothedAsymmetry.setTargetValue(targetAsymmetry);
        smoothedWarmth.setTargetValue(targetWarmth);
    }

    void processBuffer(std::vector<std::vector<float>>& buffer, int blockSize)
    {
        // kMaxPreampChannels bounds the stack pointer caches below. Indexing the
        // two std::array<float*, 2> caches by `channel` without this bound would
        // write past the end of a stack array for more than 2 channels. The
        // plugin only advertises mono/stereo (isBusesLayoutSupported), so the
        // bound is explicit rather than assumed.
        int numCh = std::min(NumChannels, static_cast<int>(buffer.size()));
        numCh = std::min(numCh, preSatBuffer.getNumChannels());
        numCh = std::min(numCh, kMaxPreampChannels);
        if (numCh <= 0 || blockSize == 0)
            return;

        int availableSamples = blockSize;
        for (size_t ch = 0; ch < static_cast<size_t>(numCh); ++ch)
            availableSamples = std::min(availableSamples, static_cast<int>(buffer[ch].size()));
        if (availableSamples <= 0)
            return;

        const int maxChunk = std::max(1, MaxBlockSize);

        const int effectiveOversampleFactor = getEffectiveOversampleFactor();
        const bool useOversampling = oversampleEnabled && (effectiveOversampleFactor > 1);
        int offset = 0;
        while (offset < availableSamples)
        {
            const int chunk = std::min(maxChunk, availableSamples - offset);

            if (useOversampling)
            {
                // Stage A: Pre-saturation (linear), plus per-sample parameter capture.
                std::array<float*, kMaxPreampChannels> preSatPtrs{};
                for (int channel = 0; channel < numCh; ++channel)
                    preSatPtrs[(size_t)channel] = preSatBuffer.getWritePointer(channel);

                for (int sample = 0; sample < chunk; ++sample)
                {
                    float inputLevel = sanitizeParam(smoothedInputLevel.getNextValue(), targetInputLevel);
                    float makeupGain = sanitizeParam(smoothedMakeupGain.getNextValue(), targetMakeupGain);
                    float asymmetry = sanitizeParam(smoothedAsymmetry.getNextValue(), targetAsymmetry);
                    float warmth = sanitizeParam(smoothedWarmth.getNextValue(), targetWarmth);

                    sampleAsymmetry[(size_t)sample] = asymmetry;
                    sampleWarmth[(size_t)sample] = warmth;
                    sampleMakeupGain[(size_t)sample] = makeupGain;

                    const auto index = static_cast<size_t>(offset + sample);
                    for (size_t ch = 0; ch < static_cast<size_t>(numCh); ++ch)
                    {
                        float in = sanitizeSample(buffer[ch][index] * inputLevel);
                        if (!Enabled)
                        {
                            buffer[ch][index] = sanitizeSample(in);
                            continue;
                        }

                        float dcBlocked = processInputCoupling(in, ch);
                        float loaded = processInputLoading(dcBlocked, ch);
                        float preFiltered = processPreSaturationLPF(loaded, ch);
                        preSatPtrs[ch][sample] = preFiltered;
                    }
                }

                if (!Enabled)
                    return;

                // Stage B: Oversampled saturation (4x) - process in-place.
                auto baseBlock = juce::dsp::AudioBlock<float>(preSatBuffer).getSubBlock(0, (size_t)chunk);
                auto osBlock = satOversampler.processSamplesUp(baseBlock);
                const int osSamples = satOversampler.getOversampledNumSamples();

                for (size_t ch = 0; ch < static_cast<size_t>(numCh); ++ch)
                {
                    float* osData = osBlock.getChannelPointer(ch);
                    for (int i = 0; i < osSamples; ++i)
                    {
                        const int baseIndex = std::min(chunk - 1, i / effectiveOversampleFactor);
                        const float asymmetry = sampleAsymmetry[(size_t)baseIndex];
                        const float warmth = sampleWarmth[(size_t)baseIndex];
                        float saturated = processBJTSaturationADAA(osData[i], ch, asymmetry, warmth);
                        float postFiltered = processPostSaturationLPF(saturated, ch, postSatLPFOsCoeff);
                        osData[i] = processPostSaturationLPFStage2(postFiltered, ch, postSatLPFOsCoeff);
                    }
                }

                satOversampler.processSamplesDown(baseBlock);

                // Stage C: Post-saturation processing at base rate.
                std::array<const float*, kMaxPreampChannels> preSatReadPtrs{};
                for (int channel = 0; channel < numCh; ++channel)
                    preSatReadPtrs[(size_t)channel] = preSatBuffer.getReadPointer(channel);

                for (int sample = 0; sample < chunk; ++sample)
                {
                    const float makeupGain = sampleMakeupGain[(size_t)sample];
                    const auto index = static_cast<size_t>(offset + sample);
                    for (size_t ch = 0; ch < static_cast<size_t>(numCh); ++ch)
                    {
                        float saturated = preSatReadPtrs[ch][sample];
                        float dcBlocked = processPostSaturationDCBlock(saturated, ch);
                        float compressed = processDynamicCompression(dcBlocked, ch);
                        float hp = processHPF(compressed, ch);
                        float mid = processMidBell(hp, ch);
                        float output = processOutputCoupling(mid, ch);
                        buffer[ch][index] = sanitizeSample(output * makeupGain);
                    }
                }
            }
            else
            {
                // Base-rate in-place processing (oversampling disabled).
                for (int sample = 0; sample < chunk; sample++)
                {
                    float inputLevel = sanitizeParam(smoothedInputLevel.getNextValue(), targetInputLevel);
                    float makeupGain = sanitizeParam(smoothedMakeupGain.getNextValue(), targetMakeupGain);
                    float asymmetry = sanitizeParam(smoothedAsymmetry.getNextValue(), targetAsymmetry);
                    float warmth = sanitizeParam(smoothedWarmth.getNextValue(), targetWarmth);

                    const auto index = static_cast<size_t>(offset + sample);
                    for (size_t ch = 0; ch < static_cast<size_t>(numCh); ++ch)
                    {
                        float in = sanitizeSample(buffer[ch][index] * inputLevel);
                        if (!Enabled)
                        {
                            buffer[ch][index] = sanitizeSample(in);
                            continue;
                        }

                        float dcBlocked = processInputCoupling(in, ch);
                        float loaded = processInputLoading(dcBlocked, ch);
                        float preFiltered = processPreSaturationLPF(loaded, ch);
                        float saturated = processBJTSaturationADAA(preFiltered, ch, asymmetry, warmth);
                        float postFiltered = processPostSaturationLPF(saturated, ch, postSatLPFBaseCoeff);
                        float postFiltered2 = processPostSaturationLPFStage2(postFiltered, ch, postSatLPFBaseCoeff);
                        float postDc = processPostSaturationDCBlock(postFiltered2, ch);
                        float compressed = processDynamicCompression(postDc, ch);
                        float hp = processHPF(compressed, ch);
                        float mid = processMidBell(hp, ch);
                        float output = processOutputCoupling(mid, ch);
                        buffer[ch][index] = sanitizeSample(output * makeupGain);
                    }
                }
            }

            offset += chunk;
        }
    }

private:
    int getEffectiveOversampleFactor() const { return oversampleEnabled ? kOversampleFactor : 1; }

    //==========================================================================
    // COUPLING CAPACITOR - DC BLOCKING
    // One-pole highpass for DC removal
    //==========================================================================

    inline float processInputCoupling(float x, size_t channel)
    {
        // One-pole highpass DC blocker
        float y = x - inputCouplingState[channel];
        inputCouplingState[channel] += inputCouplingCoeff * (x - inputCouplingState[channel]);
        inputCouplingState[channel] = flushDenormal(inputCouplingState[channel]);
        return sanitizeSample(y);
    }

    inline float processOutputCoupling(float x, size_t channel)
    {
        float y = x - outputCouplingState[channel];
        outputCouplingState[channel] += outputCouplingCoeff * (x - outputCouplingState[channel]);
        outputCouplingState[channel] = flushDenormal(outputCouplingState[channel]);
        return sanitizeSample(y);
    }

    //==========================================================================
    // INPUT LOADING SIMULATION
    // One-pole lowpass simulating RC network
    //==========================================================================

    inline float processInputLoading(float x, size_t channel)
    {
        // One-pole lowpass: y = z + x * b0, z = x * b1 - y * a1
        // Simplified to: state += coeff * (x - state)
        inputLoadingState[channel] += inputLoadingCoeff * (x - inputLoadingState[channel]);
        inputLoadingState[channel] = flushDenormal(inputLoadingState[channel]);
        return sanitizeSample(inputLoadingState[channel]);
    }

    //==========================================================================
    // PRE-SATURATION LOWPASS FILTER (ANTI-ALIASING)
    // One-pole lowpass at ~10kHz to reduce high frequencies before saturation
    //==========================================================================

    inline float processPreSaturationLPF(float x, size_t channel)
    {
        // One-pole lowpass at ~10kHz to reduce high frequencies before saturation
        preSatLPFState[channel] += preSatLPFCoeff * (x - preSatLPFState[channel]);
        preSatLPFState[channel] = flushDenormal(preSatLPFState[channel]);
        return sanitizeSample(preSatLPFState[channel]);
    }

    //==========================================================================
    // POST-SATURATION LOWPASS + DC BLOCK
    // Reduces ultrasonic content before downsampling and keeps the compressor unbiased.
    //==========================================================================

    inline float processPostSaturationLPF(float x, size_t channel, float coeff)
    {
        postSatLPFState[channel] += coeff * (x - postSatLPFState[channel]);
        postSatLPFState[channel] = flushDenormal(postSatLPFState[channel]);
        return sanitizeSample(postSatLPFState[channel]);
    }

    inline float processPostSaturationLPFStage2(float x, size_t channel, float coeff)
    {
        postSatLPFState2[channel] += coeff * (x - postSatLPFState2[channel]);
        postSatLPFState2[channel] = flushDenormal(postSatLPFState2[channel]);
        return sanitizeSample(postSatLPFState2[channel]);
    }

    inline float processPostSaturationDCBlock(float x, size_t channel)
    {
        float y = x - postSatDCState[channel];
        postSatDCState[channel] += postSatDCCoeff * (x - postSatDCState[channel]);
        postSatDCState[channel] = flushDenormal(postSatDCState[channel]);
        return sanitizeSample(y);
    }

    //==========================================================================
    // ADAA SATURATION - the live saturation path, called by processBuffer() on
    // every sample.
    //
    // First-order ADAA per Parker et al., "Reducing the Aliasing of
    // Nonlinear Waveshaping Using Continuous-Time Convolution" (DAFx 2019)
    // Asymmetric soft-clipping for BJT transistor character
    //==========================================================================

    // First-order ADAA. The divided difference is evaluated in DOUBLE, and that
    // is not cosmetic: evaluated in float32 it is the single largest source of
    // distortion in this processor.
    //
    // (AD1(x) - AD1(x1)) / (x - x1) is a catastrophic cancellation when x and x1
    // are close: the two antiderivatives agree to many digits and their
    // difference is then divided by a small number, amplifying the rounding
    // error of whatever precision the subtraction happened in. In float32 the
    // resulting error is large enough that the ADAA branch and the midpoint
    // fallback branch disagree audibly, and the per-sample switching between
    // them at |x - x1| = ADAA_TOL injects harmonics.
    //
    // Measured on a 1 kHz sine at 48 kHz with 4x oversampling and the divided
    // difference in float32:
    //     input amplitude 0.0003 -> THD 0.0004%   (every sample takes the
    //                                              fallback branch: clean)
    //     input amplitude 0.001  -> THD 14.2%     (branches mixed: worst case)
    //     input amplitude 0.003  -> THD 3.1%
    //     input amplitude 0.01   -> THD 0.28%
    // The peak sits where the sine's per-sample slew straddles ADAA_TOL, which
    // is exactly the signature of a branch mismatch rather than of the intended
    // saturation. Around -60 dBFS - ordinary delay/reverb tail level - a
    // float32 divided difference adds 14% distortion to the whole wet path.
    //
    // In double the cancellation error is ~2e-15 rather than ~6e-8, the two
    // branches agree, and the same sweep measures below 0.01% everywhere the
    // signal is not actually being driven into saturation.
    inline float processBJTSaturationADAA(float x, size_t channel, float asymmetry, float warmth)
    {
        const double xd = static_cast<double>(x);
        const double x1d = static_cast<double>(adaaX1[channel]);
        const double asym = static_cast<double>(asymmetry);
        const double warm = static_cast<double>(warmth);

        // Ill-conditioning check (standard ADAA tolerance)
        const bool illCondition = std::abs(xd - x1d) < static_cast<double>(ADAA_TOL);

        double y;
        if (illCondition)
        {
            // Fallback: evaluate at midpoint
            y = bjtSaturationD(0.5 * (xd + x1d), asym, warm);
        }
        else
        {
            // ADAA formula: (AD1(x) - AD1(x1)) / (x - x1)
            //
            // Recompute the previous antiderivative with CURRENT parameters
            // rather than caching it, so a change in asymmetry/warmth cannot
            // introduce a discontinuity between consecutive samples.
            const double ad1_x = bjtSaturationAD1D(xd, asym, warm);
            const double ad1_x1 = bjtSaturationAD1D(x1d, asym, warm);
            y = (ad1_x - ad1_x1) / (xd - x1d);
        }

        // Update state - only store x, not the antiderivative (we recompute it)
        adaaX1[channel] = x;

        return sanitizeSample(static_cast<float>(y));
    }

    // Numerically stable log(cosh(x)).
    //
    // std::log(std::cosh(x)) overflows to inf for |x| > ~710 in double (and
    // ~89 in float), and log(inf) - log(inf) downstream is NaN. The identity
    //     log(cosh(x)) = |x| + log1p(exp(-2|x|)) - log(2)
    // is exact for all x, never overflows, and is more accurate near zero
    // because log1p is.
    static inline double logCosh(double x) noexcept
    {
        const double ax = std::abs(x);
        return ax + std::log1p(std::exp(-2.0 * ax)) - 0.69314718055994530942;
    }

    // Asymmetric BJT saturation function.
    // - DC bias creates asymmetry -> even harmonics (2nd, 4th).
    // - tanh(asymmetry) is subtracted to remove the DC offset; without it a
    //   change in asymmetry creates a DC transient even with no input.
    // - The warmth term injects an explicit 2nd harmonic via x*|x|.
    //
    // Evaluated in double because the ADAA divided difference is (see
    // processBJTSaturationADAA); both branches have to use the same precision
    // or the branch switch reintroduces that mismatch.
    inline double bjtSaturationD(double x, double asymmetry, double warmth) const
    {
        const double sat = std::tanh(x + asymmetry) - std::tanh(asymmetry);
        const double h2 = warmth * x * std::abs(x);
        return sat + h2;
    }

    // First antiderivative of BJT saturation
    // Pattern from TanhNL.h lines 37-39: antiderivative of tanh is log(cosh)
    // Extended for asymmetric function with DC removal
    inline double bjtSaturationAD1D(double x, double asymmetry, double warmth) const
    {
        const double biased = x + asymmetry;

        // Antiderivative of [tanh(x+A) - tanh(A)] = log(cosh(x+A)) - x*tanh(A)
        // The -x*tanh(A) term accounts for the DC offset removal
        const double tanhAD = logCosh(biased) - x * std::tanh(asymmetry);

        // Antiderivative of W*x*|x| = W*|x|^3/3
        const double absX = std::abs(x);
        const double h2AD = warmth * absX * absX * absX / 3.0;

        return tanhAD + h2AD;
    }

    //==========================================================================
    // DYNAMIC COMPRESSION
    // Standard envelope follower with attack/release
    //==========================================================================

    inline float processDynamicCompression(float x, size_t channel)
    {
        float inputLevel = std::abs(x);

        // Envelope follower with separate attack/release coefficients
        if (inputLevel > compressionEnvelope[channel])
        {
            // Attack phase
            compressionEnvelope[channel] += compressionAttackCoeff * (inputLevel - compressionEnvelope[channel]);
        }
        else
        {
            // Release phase
            compressionEnvelope[channel] += compressionReleaseCoeff * (inputLevel - compressionEnvelope[channel]);
        }

        compressionEnvelope[channel] = flushDenormal(compressionEnvelope[channel]);

        // GATE: Don't compress signals below noise floor threshold
        // This prevents the compressor from amplifying tape hiss
        // ~-60 dB threshold - signals below this pass through unchanged
        const float noiseFloorThreshold = 0.001f;
        if (compressionEnvelope[channel] < noiseFloorThreshold)
        {
            return sanitizeSample(x);  // Pass through - don't compress noise
        }

        // Soft compression curve (gentle ratio)
        float gain = 1.0f / (1.0f + compressionAmount * compressionEnvelope[channel]);

        return sanitizeSample(x * gain);
    }

    //==========================================================================
    // TDF-II BIQUAD FILTERS
    // Standard Transposed Direct Form II implementation
    // Uses 2 state variables (z1, z2) per filter
    // More numerically stable than Direct Form I
    //==========================================================================

    // High-pass filter 50Hz
    //
    // The z-state flushes matter: without denormal and non-finite handling a
    // NaN reaching these two biquads would latch into z1/z2 permanently AND
    // propagate NaN to the plugin output (there is no sanitizeSample on the way
    // out of here). Costs two compares per sample; identical output for finite
    // signals.
    inline float processHPF(float x, size_t channel)
    {
        // TDF-II biquad: y = b0*x + z1; z1 = b1*x - a1*y + z2; z2 = b2*x - a2*y
        float y = hpfB0 * x + hpfZ1[channel];
        hpfZ1[channel] = flushDenormal(hpfB1 * x - hpfA1 * y + hpfZ2[channel]);
        hpfZ2[channel] = flushDenormal(hpfB2 * x - hpfA2 * y);
        return sanitizeSample(y);
    }

    // Mid bell boost 700Hz, +2dB, Q=0.7
    inline float processMidBell(float x, size_t channel)
    {
        float y = midB0 * x + midZ1[channel];
        midZ1[channel] = flushDenormal(midB1 * x - midA1 * y + midZ2[channel]);
        midZ2[channel] = flushDenormal(midB2 * x - midA2 * y);
        return sanitizeSample(y);
    }

    //==========================================================================
    // FILTER COEFFICIENT CALCULATION
    // Formulas from Robert Bristow-Johnson Audio EQ Cookbook
    //==========================================================================

    void updateFilters()
    {
        if (SampleRate <= 0.0f)
            return;

        constexpr float pi = juce::MathConstants<float>::pi;

        // Pre-saturation LPF (~10kHz)
        float preSatCutoff = 10000.0f;
        preSatLPFCoeff = 1.0f - std::exp(-2.0f * pi * preSatCutoff / SampleRate);

        // Post-saturation LPF:
        // - Base path keeps a wide cutoff.
        // - Oversampled path uses a lower cutoff in base-rate terms to suppress
        //   foldback before decimation.
        const float postSatCutoffBase = 0.40f * SampleRate;
        // Keep oversampled branch tonal transfer closer to base path while relying
        // on oversampler decimation filters for Nyquist rejection.
        const float postSatCutoffOsBaseRef = std::min(22000.0f, 0.45f * SampleRate);
        postSatLPFBaseCoeff = 1.0f - std::exp(-2.0f * pi * postSatCutoffBase / SampleRate);
        const float osRate = SampleRate * (float)getEffectiveOversampleFactor();
        postSatLPFOsCoeff = 1.0f - std::exp(-2.0f * pi * postSatCutoffOsBaseRef / osRate);

        // Post-saturation DC block (gentle highpass)
        float postSatDcFc = 10.0f;
        postSatDCCoeff = 1.0f - std::exp(-2.0f * pi * postSatDcFc / SampleRate);

        // Input coupling cap ~25Hz one-pole highpass
        float hpFc = 25.0f;
        inputCouplingCoeff = 1.0f - std::exp(-2.0f * pi * hpFc / SampleRate);

        // Output coupling cap ~20Hz one-pole highpass
        hpFc = 20.0f;
        outputCouplingCoeff = 1.0f - std::exp(-2.0f * pi * hpFc / SampleRate);

        // The process recurrence is a one-pole exponential smoother, so its
        // coefficient must use the matched-pole form. A bilinear-transform
        // coefficient does not fit this recurrence and would put the declared
        // 5 kHz loading corner near 2 kHz.
        constexpr float inputLoadingCutoffHz = 5000.0f;
        inputLoadingCoeff = 1.0f - std::exp(-2.0f * pi * inputLoadingCutoffHz / SampleRate);

        // Compression envelope coefficients
        // Attack ~1ms, Release ~100ms
        compressionAttackCoeff = 1.0f - std::exp(-1.0f / (SampleRate * 0.001f));
        compressionReleaseCoeff = 1.0f - std::exp(-1.0f / (SampleRate * 0.100f));

        // === HPF 50Hz Biquad (Butterworth Q=0.707) ===
        {
            float fc = 50.0f;
            float Q = 0.707f;
            float w0 = 2.0f * pi * fc / SampleRate;
            float cosw0 = std::cos(w0);
            float sinw0 = std::sin(w0);
            float alpha = sinw0 / (2.0f * Q);

            float a0 = 1.0f + alpha;
            hpfB0 = ((1.0f + cosw0) / 2.0f) / a0;
            hpfB1 = (-(1.0f + cosw0)) / a0;
            hpfB2 = hpfB0;
            hpfA1 = (-2.0f * cosw0) / a0;
            hpfA2 = (1.0f - alpha) / a0;
        }

        // === Mid Bell 700Hz, +2dB, Q=0.7 Biquad ===
        // Peaking EQ formula from Audio EQ Cookbook
        {
            float fc = 700.0f;
            float gainDb = 2.0f;
            float Q = 0.7f;

            float w0 = 2.0f * pi * fc / SampleRate;
            float A = std::pow(10.0f, gainDb / 40.0f);
            float cosw0 = std::cos(w0);
            float sinw0 = std::sin(w0);
            float alpha = sinw0 / (2.0f * Q);

            float a0 = 1.0f + alpha / A;
            midB0 = (1.0f + alpha * A) / a0;
            midB1 = (-2.0f * cosw0) / a0;
            midB2 = (1.0f - alpha * A) / a0;
            midA1 = midB1;  // For peaking filter, a1 = b1
            midA2 = (1.0f - alpha / A) / a0;
        }
    }

    // Parameters
    bool Enabled = true;
    float DriveDb = 0.0f;

    // Target values (set by updateParameters)
    float targetMakeupGain = 1.0f;
    float targetInputLevel = 1.0f;
    float targetAsymmetry = 0.1f;
    float targetWarmth = 0.1f;

    // RT-safety thresholds
    static constexpr int kMaxPreampChannels = 8;
    static constexpr float maxInputGain = 4.0f;
    static constexpr float maxDriveDb = 12.0f;
    static constexpr float inputLevelEpsilon = 1.0e-5f;
    static constexpr float denormalEpsilon = 1.0e-20f;

    inline float sanitizeParam(float value, float fallback) const { return std::isfinite(value) ? value : fallback; }

    // Flushes denormals AND non-finite values to zero.
    //
    // The non-finite half is the important part. Every recursive state update in
    // this class is written as
    //     state += coeff * (x - state);  state = flushDenormal(state);
    //     return sanitizeSample(state);
    // and a test of only `std::abs(value) < eps` is FALSE for NaN. With that
    // test alone a single NaN entering any of these filters would latch into
    // the state permanently: sanitizeSample would clean the returned sample,
    // which makes the output look safe, while the state stayed NaN and the
    // preamp stayed silent until the auto-panic clearAudioState() path ran.
    //
    // For every finite input the non-finite check is a no-op.
    inline float flushDenormal(float value) const
    {
        return (std::isfinite(value) && std::abs(value) >= denormalEpsilon) ? value : 0.0f;
    }

    inline float sanitizeSample(float value) const
    {
        if (!std::isfinite(value))
            return 0.0f;
        return flushDenormal(value);
    }

    // Smoothed values (used in processing) - prevents zipper noise and pops
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> smoothedMakeupGain;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> smoothedInputLevel;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedAsymmetry;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedWarmth;

    static constexpr float smoothingTimeSeconds = 0.005f;

    float SampleRate = 44100.0f;
    int NumChannels = 2;
    int MaxBlockSize = 512;

    // ADAA tolerance for ill-conditioning check
    static constexpr float ADAA_TOL = 1.0e-5f;

    // Compression parameters
    float compressionAmount = 0.3f;
    float compressionAttackCoeff = 0.0f;
    float compressionReleaseCoeff = 0.0f;

    // Coupling capacitor coefficients
    float inputCouplingCoeff = 0.01f;
    float outputCouplingCoeff = 0.01f;

    // Input loading coefficient
    float inputLoadingCoeff = 0.5f;

    // Pre-saturation LPF coefficient (anti-aliasing filter ~10kHz)
    float preSatLPFCoeff = 0.5f;
    float postSatLPFBaseCoeff = 0.5f;
    float postSatLPFOsCoeff = 0.5f;
    float postSatDCCoeff = 0.01f;

    // Oversampling for saturation stage
    static constexpr int kOversampleFactor = 4;
    bool oversampleEnabled = true;
    echoes::IIROversampler satOversampler;
    juce::AudioBuffer<float> preSatBuffer;
    std::vector<float> sampleAsymmetry;
    std::vector<float> sampleWarmth;
    std::vector<float> sampleMakeupGain;

    // Per-channel state vectors
    std::vector<float> inputCouplingState;
    std::vector<float> inputLoadingState;
    std::vector<float> preSatLPFState;
    std::vector<float> postSatLPFState;
    std::vector<float> postSatLPFState2;
    std::vector<float> postSatDCState;
    std::vector<float> compressionEnvelope;
    std::vector<float> outputCouplingState;

    // ADAA state vector. Only the previous INPUT is stored; the previous
    // antiderivative is recomputed each sample with current parameters, so no
    // companion antiderivative vector is stored.
    std::vector<float> adaaX1;

    // TDF-II biquad state vectors (per channel)
    std::vector<float> hpfZ1, hpfZ2;
    std::vector<float> midZ1, midZ2;

    // HPF biquad coefficients
    float hpfB0 = 1.0f, hpfB1 = 0.0f, hpfB2 = 0.0f;
    float hpfA1 = 0.0f, hpfA2 = 0.0f;

    // Mid bell biquad coefficients
    float midB0 = 1.0f, midB1 = 0.0f, midB2 = 0.0f;
    float midA1 = 0.0f, midA2 = 0.0f;
};
