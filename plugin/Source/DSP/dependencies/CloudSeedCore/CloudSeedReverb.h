// CloudSeedReverb: thin wrapper adapting Cloudseed::ReverbController
// for use inside Spatial Echo's ReverbProcessor.
//
// Responsibilities:
//   1. Define BUFFER_SIZE before any CloudSeed headers
//   2. Map plugin-level parameters to CloudSeed's 0-1 parameter array
//   3. Provide Reset / UpdateParameters / Process matching ReverbProcessor's needs
//   4. Feedback loop HF damping via CloudSeed's per-DelayLine EQ
//
// HF damping rationale:
//   CloudSeed's DelayLine::Process applies lowShelf / highShelf / lowPass
//   filters INSIDE the feedback loop.  Each iteration attenuates HF, which
//   accumulates into the natural "darker over time" decay of real spaces.
//   Without this, HF energy recirculates indefinitely and builds up into
//   a harsh, electric-sounding character.  This is the standard mechanism
//   professional reverbs use for their Damping / Tone controls.

#pragma once

#include "CloudSeedConfig.h"          // defines BUFFER_SIZE, MAX_STR_SIZE
#include "../ThirdPartyIncludesBegin.h"
#include "DSP/ReverbController.h"
#include "../ThirdPartyIncludesEnd.h"
#include <juce_dsp/juce_dsp.h>

#include <vector>
#include <memory>
#include <cstring>
#include <cmath>
#include <algorithm>

class CloudSeedReverb
{
public:
    CloudSeedReverb()
        : controller(48000)
    {
        currentSampleRate = 48000.0f;
        applyDefaults();
    }

    // Called from ReverbProcessor::Reset
    void reset(float sampleRate, int maxBlockSize)
    {
        maxBlockSize = std::max(1, maxBlockSize);

        // Run the reverb engine at 2x sample rate for anti-aliasing.
        // Modulated delay lines with linear interpolation create aliasing
        // that folds back from Nyquist; 2x oversampling pushes the
        // foldback above 22kHz (inaudible).
        internalSampleRate = sampleRate * 2.0f;
        controller.SetSamplerate(static_cast<int>(internalSampleRate));
        controller.ClearBuffers();

        currentSampleRate = sampleRate;

        oversampler = std::make_unique<juce::dsp::Oversampling<float>>(
            2,
            1,
            juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
            true);
        oversampler->initProcessing(static_cast<size_t>(maxBlockSize));
        oversampler->reset();
        oversamplerMaxBlockSize = maxBlockSize;

        ioBuffer.setSize(2, maxBlockSize, false, false, true);
        ioBuffer.clear();

        // Pre-allocate 2x oversampled output buffers.
        const size_t os2 = static_cast<size_t>(maxBlockSize) * 2;
        oversampledOutL.assign(os2, 0.0f);
        oversampledOutR.assign(os2, 0.0f);
    }

    // Map plugin knobs to CloudSeed's 0-1 parameter space.
    // All values arrive in their APVTS ranges and are normalised here.
    void updateParameters(float size,        // 0.0 – 1.0 (maps to LateLineSize)
                          float decay,       // 0.0 – 1.0 (maps to LateLineDecay)
                          float damping,     // 0.0 – 1.0 (inverted → HighCut)
                          float diffusion,   // 0.0 – 1.0
                          float modRate,     // 0.0 – 1.0
                          float modDepth,    // 0.0 – 1.0
                          float lowCut,      // Hz 20-1000 → normalised
                          float highCut,     // Hz 400-20000 → normalised
                          float preDelay,    // 0.0 – 1.0 (maps to TapPredelay)
                          float earlyLateMix,// 0.0 – 1.0
                          int   lineCount,   // 1-12
                          float stereoWidth) // 0.0 – 1.0 (maps to EqCrossSeed)
    {
        using namespace Cloudseed;

        const float userHighCutNorm = frequencyToCloudCutoffNorm(highCut);

        // --- Space ---
        controller.SetParameter(Parameter::LateLineSize,  size);

        // Both engines receive the same nominal T60 target. Cloud's nested
        // allpasses, modulation and damping make the realised decay depend on
        // frequency; the scalar loop gain is an average-time approximation.
        //
        // The Spring engine maps its shaped decay parameter to seconds as
        //   t60 = 0.4 * 20^decay        (TripleSpringReverb::setParams)
        // giving 0.4 s .. 8 s. This used to hand the same 0..1 value straight
        // to CloudSeed's LateLineDecay, whose own scaling is
        //   seconds = 0.05 + Resp3dec(v) * 59.95
        // i.e. 0.05 s .. 60 s. The two engines therefore ran completely
        // different decay laws behind one Decay macro: at the default the
        // Spring asked for 2.64 s while Cloud asked for 4.63 s, and at Decay
        // 1.0 the Spring asked for 8 s while Cloud asked for 60 s.
        //
        // Measured consequence (ReverbEngineLevelParityProbe, late window
        // 650-950 ms): Spring sat up to 22.5 dB below Cloud, worst at short
        // decays. That number was being printed as an ungated "(report)" field
        // and so never failed anything.
        //
        // Convert to seconds with the Spring's law, then invert CloudSeed's
        // scaling so both engines are asked for the same T60.
        controller.SetParameter(Parameter::LateLineDecay,
                                secondsToCloudDecayNorm(springT60Seconds(decay)));

        // Spatial Echo's public macro is linear 0..200 ms. CloudSeed expands
        // TapPredelay with Resp1dec(x) * 500 ms, so invert that native law:
        // x = log10(1 + 9 * (desiredMs / 500)), desiredMs = 200 * macro.
        const float clampedPreDelay = std::clamp(preDelay, 0.0f, 1.0f);
        const float cloudPreDelay = std::log10(1.0f + 3.6f * clampedPreDelay);
        controller.SetParameter(Parameter::TapPredelay, cloudPreDelay);

        // Line count: normalise 1-12 → 0-1
        float lineCountNorm = static_cast<float>(lineCount - 1) / 11.0f;
        if (lineCountNorm < 0.0f) lineCountNorm = 0.0f;
        if (lineCountNorm > 1.0f) lineCountNorm = 1.0f;
        controller.SetParameter(Parameter::LateLineCount, lineCountNorm);

        // --- Tone / Damping ---
        // Input filter: high damping = lower HighCut on input path.
        // Cap at 0.88 (~16kHz): all of CloudSeed's own factory presets use 3-17kHz,
        // never 20kHz.  The one-pole Lp1 is only -6dB/oct so it needs a
        // reasonable cutoff to provide meaningful HF attenuation on the
        // first-pass transient (before feedback loop EQ kicks in).
        float inputCutoff = std::min(0.88f - damping * 0.73f, userHighCutNorm);
        controller.SetParameter(Parameter::HighCut, inputCutoff);
        controller.SetParameter(Parameter::HighCutEnabled, 1.0);

        // Feedback loop damping (the critical fix for "electric" quality):
        // EqLowpass is a one-pole LPF applied INSIDE each DelayLine's feedback
        // loop.  Every iteration attenuates HF, accumulating into natural decay.
        // EqCutoff range: 400 + Resp4oct(val) * 19600 → 400Hz–20kHz
        //   damping=0 → cutoff norm 0.90 → ~18kHz (very open)
        //   damping=0.5 → cutoff norm 0.52 → ~4.5kHz (moderate)
        //   damping=1.0 → cutoff norm 0.15 → ~800Hz (very dark)
        controller.SetParameter(Parameter::EqLowpassEnabled, 1.0);
        float fbCutoff = std::min(0.90f - damping * 0.75f, userHighCutNorm);
        controller.SetParameter(Parameter::EqCutoff, fbCutoff);

        // High shelf inside feedback loop for additional gentle HF rolloff.
        // EqHighGain range: -20 + val*20 → -20dB to 0dB
        //   damping=0 → -1dB (almost transparent)
        //   damping=1.0 → -8dB (significant per-iteration cut)
        controller.SetParameter(Parameter::EqHighShelfEnabled, 1.0);
        controller.SetParameter(Parameter::EqHighFreq, 0.35);  // ~3kHz shelf corner
        float shelfGainDb = -1.0f - damping * 7.0f;  // -1dB to -8dB
        float shelfGainNorm = (shelfGainDb + 20.0f) / 20.0f;  // normalise to 0-1
        controller.SetParameter(Parameter::EqHighGain, shelfGainNorm);

        // Invert the native four-octave law so the Hz cutoff means the same
        // thing here as in Spring. Linear normalization mapped 150 Hz to 49 Hz.
        controller.SetParameter(Parameter::LowCut, frequencyToCloudLowCutNorm(lowCut));
        controller.SetParameter(Parameter::LowCutEnabled, 1.0);

        // --- Texture ---
        // Root cause of "electric/metallic" quality (per Sean Costello / Valhalla DSP):
        // Series allpass delays with high feedback create metallic resonance.
        // Fix: reduce feedback coefficients, cap stage count, use longer delays.
        controller.SetParameter(Parameter::EarlyDiffuseEnabled, 1.0);
        // Cap feedback at 0.5: raw diffusion (up to 0.9) is way too resonant
        controller.SetParameter(Parameter::EarlyDiffuseFeedback, diffusion * 0.55f);
        // Minimum 3 stages (norm 0.167 → scaled 1+0.167*12=3) even at low diffusion,
        // otherwise Sparse gives only 1 stage = raw discrete echoes = "electric" burst.
        // Cap at 5 stages max (norm 0.33 → scaled 1+0.33*12=5).
        float earlyStages = std::max(0.167f, std::min(diffusion * 0.5f, 0.33f));
        controller.SetParameter(Parameter::EarlyDiffuseCount, earlyStages);
        // Longer delays push resonance frequency lower (less audible coloration)
        controller.SetParameter(Parameter::EarlyDiffuseDelay, 0.7);

        controller.SetParameter(Parameter::LateDiffuseEnabled, 1.0);
        controller.SetParameter(Parameter::LateDiffuseFeedback, diffusion * 0.45f);
        // 6 stages in late diffuser for density without excessive ringing
        controller.SetParameter(Parameter::LateDiffuseCount, 0.65);
        // Longer late diffuse delays for smoother tail
        controller.SetParameter(Parameter::LateDiffuseDelay, 0.5 + diffusion * 0.3);

        // --- Motion ---
        controller.SetParameter(Parameter::LateLineModAmount, modDepth);
        controller.SetParameter(Parameter::LateLineModRate, modRate);
        controller.SetParameter(Parameter::EarlyDiffuseModAmount, modDepth * 0.5f);
        controller.SetParameter(Parameter::EarlyDiffuseModRate, modRate);
        // Wire late diffuse mod to user controls (were orphaned at defaults!)
        controller.SetParameter(Parameter::LateDiffuseModAmount, modDepth * 0.7f);
        controller.SetParameter(Parameter::LateDiffuseModRate, modRate * 0.8f);

        // --- Mix (Early vs Late) ---
        // earlyLateMix 0 = all early, 1 = all late, equal power in between.
        //
        // The previous mapping drove CloudSeed's two -30..0 dB output faders
        // linearly in dB from the same control:
        //     earlyDb = -30 + (1 - mix) * 30,  lateDb = -30 + mix * 30
        // That is not a crossfade: at the default mix of 0.704 it delivered
        // early at -21.1 dB and late at -8.9 dB, for a combined -8.6 dB of
        // output attenuation that varied with the Size macro (Size drives
        // earlyLateMix). It also disagreed with the Spring engine, which uses
        // the cos/sin equal-power law below, so the two engines could not sit
        // at the same level for the same macro settings.
        const float mixClamped = std::clamp(earlyLateMix, 0.0f, 1.0f);
        const float earlyGain = std::cos(mixClamped * 1.57079632679f);
        const float lateGain  = std::sin(mixClamped * 1.57079632679f);
        controller.SetParameter(Parameter::EarlyOut, gainToCloudOutNorm(earlyGain));
        controller.SetParameter(Parameter::LateOut,  gainToCloudOutNorm(lateGain));
        controller.SetParameter(Parameter::DryOut,   0.0);  // dry mixed externally by ReverbProcessor

        // --- Stereo Width ---
        // CloudSeed's cross-seed/input-mix parameters narrow as they rise.
        // NOTE: these saturate early (mono input makes InputMix inert and the
        // seed decorrelation maxes out quickly), so the authoritative Width
        // control is the mid/side stage in ReverbProcessor's cloud path; this
        // internal mapping only shapes the tank's native seed character.
        const float narrowness = 1.0f - std::clamp(stereoWidth, 0.0f, 1.0f);
        controller.SetParameter(Parameter::EqCrossSeed, narrowness);
        controller.SetParameter(Parameter::InputMix, narrowness);

        // --- Multitap (always on for richness) ---
        controller.SetParameter(Parameter::TapEnabled, 1.0);
        controller.SetParameter(Parameter::TapCount, 0.3);
        controller.SetParameter(Parameter::TapDecay, 0.5);
        controller.SetParameter(Parameter::TapLength, 0.3);
    }

    // Process stereo audio in-place with 2x internal oversampling.
    // buffer[0] = left channel, buffer[1] = right channel (if present).
    void process(std::vector<std::vector<float>>& buffer, int blockSize)
    {
        const int numCh = static_cast<int>(buffer.size());
        if (numCh == 0 || blockSize <= 0) return;

        const int osBlockSize = blockSize * 2;

        auto clearOutput = [&]()
        {
            for (auto& channel : buffer)
            {
                const int samplesToClear = std::min(blockSize, static_cast<int>(channel.size()));
                std::fill(channel.begin(), channel.begin() + samplesToClear, 0.0f);
            }
        };

        if (oversampler == nullptr ||
            ioBuffer.getNumChannels() < 2 ||
            ioBuffer.getNumSamples() < blockSize ||
            blockSize > oversamplerMaxBlockSize ||
            static_cast<int>(oversampledOutL.size()) < osBlockSize ||
            static_cast<int>(oversampledOutR.size()) < osBlockSize)
        {
            jassertfalse;
            clearOutput();
            return;
        }

        float* inL = buffer[0].data();
        float* inR = (numCh >= 2) ? buffer[1].data() : buffer[0].data();

        auto* ioL = ioBuffer.getWritePointer(0);
        auto* ioR = ioBuffer.getWritePointer(1);
        std::memcpy(ioL, inL, static_cast<size_t>(blockSize) * sizeof(float));
        if (numCh >= 2)
            std::memcpy(ioR, inR, static_cast<size_t>(blockSize) * sizeof(float));
        else
            std::memcpy(ioR, inL, static_cast<size_t>(blockSize) * sizeof(float));

        juce::dsp::AudioBlock<float> ioBlock(ioBuffer);
        auto baseRateBlock = ioBlock.getSubBlock(0, static_cast<size_t>(blockSize));
        auto oversampledBlock = oversampler->processSamplesUp(baseRateBlock);

        float* osInL = oversampledBlock.getChannelPointer(0);
        float* osInR = oversampledBlock.getChannelPointer(1);

        // Process reverb at 2x sample rate.
        controller.Process(osInL, osInR,
                           oversampledOutL.data(), oversampledOutR.data(),
                           osBlockSize);

        // Write processed 2x samples back into oversampler output block for downsampling.
        std::memcpy(osInL, oversampledOutL.data(), static_cast<size_t>(osBlockSize) * sizeof(float));
        std::memcpy(osInR, oversampledOutR.data(), static_cast<size_t>(osBlockSize) * sizeof(float));

        oversampler->processSamplesDown(baseRateBlock);

        const float* outL = ioBuffer.getReadPointer(0);
        const float* outR = ioBuffer.getReadPointer(1);
        std::memcpy(buffer[0].data(), outL, static_cast<size_t>(blockSize) * sizeof(float));
        if (numCh >= 2)
            std::memcpy(buffer[1].data(), outR, static_cast<size_t>(blockSize) * sizeof(float));
        else
        {
            float* monoOut = buffer[0].data();
            for (int i = 0; i < blockSize; ++i)
                monoOut[i] = 0.5f * (outL[i] + outR[i]);
        }
    }

    void clearBuffers()
    {
        controller.ClearBuffers();
        if (oversampler != nullptr)
            oversampler->reset();
        ioBuffer.clear();
    }

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    double getInputLowCutHzForTesting()
    {
        return Cloudseed::ScaleParam(
            controller.GetAllParameters()[Cloudseed::Parameter::LowCut],
            Cloudseed::Parameter::LowCut);
    }
#endif

private:
    Cloudseed::ReverbController controller;
    juce::AudioBuffer<float> ioBuffer;     // Base-rate stereo input/output
    std::vector<float> oversampledOutL;    // 2x oversampled output (L)
    std::vector<float> oversampledOutR;    // 2x oversampled output (R)
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    int oversamplerMaxBlockSize = 0;
    float currentSampleRate = 48000.0f;
    float internalSampleRate = 96000.0f;   // 2x currentSampleRate

    // The single shared decay law, in seconds. Must stay identical to
    // TripleSpringReverb::setParams (minT60 0.4 s, maxT60 8.0 s).
    static float springT60Seconds(float shapedDecay)
    {
        constexpr float kMinT60 = 0.4f;
        constexpr float kMaxT60 = 8.0f;
        return kMinT60 * std::pow(kMaxT60 / kMinT60,
                                  std::clamp(shapedDecay, 0.0f, 1.0f));
    }

    // Invert CloudSeed's LateLineDecay scaling:
    //   seconds = 0.05 + ((10^(3v) - 1) * (1000/999) * 0.001) * 59.95
    // so   v = log10(1 + (seconds - 0.05) / 0.0600100...) / 3
    static double secondsToCloudDecayNorm(float seconds)
    {
        constexpr float kFloorSeconds = 0.05f;
        constexpr float kSpan = 59.95f;
        const float dec3Mult = (1000.0f / 999.0f) * 0.001f;
        const float scale = kSpan * dec3Mult;                 // ~0.06001
        const float excess = std::max(0.0f, seconds - kFloorSeconds);
        const float v = std::log10(1.0f + excess / scale) / 3.0f;
        return std::clamp(v, 0.0f, 1.0f);
    }

    // CloudSeed's DryOut/EarlyOut/LateOut parameters scale as
    //   dB = -30 + norm * 30,  with dB <= -30 meaning silence.
    // Invert that so callers can ask for a linear gain directly.
    static double gainToCloudOutNorm(float linearGain)
    {
        if (linearGain <= 1.0e-4f)   // -80 dB; below CloudSeed's -30 dB floor
            return 0.0;
        const float db = 20.0f * std::log10(linearGain);
        return std::clamp((db + 30.0f) / 30.0f, 0.0f, 1.0f);
    }

    static float frequencyToCloudCutoffNorm(float frequencyHz)
    {
        const float clampedHz = std::clamp(frequencyHz, 400.0f, 20000.0f);
        const float response = (clampedHz - 400.0f) / 19600.0f;
        return std::clamp(0.25f * std::log2(1.0f + response * 15.0f), 0.0f, 1.0f);
    }

    static float frequencyToCloudLowCutNorm(float frequencyHz)
    {
        const float clampedHz = std::clamp(frequencyHz, 20.0f, 1000.0f);
        const float response = (clampedHz - 20.0f) / 980.0f;
        return std::clamp(0.25f * std::log2(1.0f + response * 15.0f), 0.0f, 1.0f);
    }

    // Sensible starting defaults (Dark Plate-inspired)
    void applyDefaults()
    {
        using namespace Cloudseed;
        controller.SetParameter(Parameter::Interpolation, 1.0);

        controller.SetParameter(Parameter::LowCutEnabled, 1.0);
        controller.SetParameter(Parameter::HighCutEnabled, 1.0);
        controller.SetParameter(Parameter::LowCut, 0.0);
        controller.SetParameter(Parameter::HighCut, 0.7);

        controller.SetParameter(Parameter::InputMix, 0.0);
        controller.SetParameter(Parameter::DryOut, 0.0);
        controller.SetParameter(Parameter::EarlyOut, 0.8);
        controller.SetParameter(Parameter::LateOut, 1.0);

        controller.SetParameter(Parameter::TapEnabled, 1.0);
        controller.SetParameter(Parameter::TapCount, 0.3);
        controller.SetParameter(Parameter::TapDecay, 0.5);
        controller.SetParameter(Parameter::TapPredelay, 0.1);
        controller.SetParameter(Parameter::TapLength, 0.3);

        controller.SetParameter(Parameter::EarlyDiffuseEnabled, 1.0);
        controller.SetParameter(Parameter::EarlyDiffuseCount, 0.6);
        controller.SetParameter(Parameter::EarlyDiffuseDelay, 0.5);
        controller.SetParameter(Parameter::EarlyDiffuseModAmount, 0.3);
        controller.SetParameter(Parameter::EarlyDiffuseFeedback, 0.6);
        controller.SetParameter(Parameter::EarlyDiffuseModRate, 0.3);

        controller.SetParameter(Parameter::LateMode, 1.0);
        controller.SetParameter(Parameter::LateLineCount, 0.65);
        controller.SetParameter(Parameter::LateDiffuseEnabled, 1.0);
        controller.SetParameter(Parameter::LateDiffuseCount, 0.5);
        controller.SetParameter(Parameter::LateLineSize, 0.6);
        controller.SetParameter(Parameter::LateLineModAmount, 0.3);
        controller.SetParameter(Parameter::LateDiffuseDelay, 0.5);
        controller.SetParameter(Parameter::LateDiffuseModAmount, 0.2);
        controller.SetParameter(Parameter::LateLineDecay, 0.5);
        controller.SetParameter(Parameter::LateLineModRate, 0.3);
        controller.SetParameter(Parameter::LateDiffuseFeedback, 0.6);
        controller.SetParameter(Parameter::LateDiffuseModRate, 0.3);

        // Feedback loop EQ, enabled by default for HF damping.
        // Actual cutoff/gain set by updateParameters() based on damping knob.
        controller.SetParameter(Parameter::EqLowShelfEnabled, 0.0);
        controller.SetParameter(Parameter::EqHighShelfEnabled, 1.0);
        controller.SetParameter(Parameter::EqHighFreq, 0.35);
        controller.SetParameter(Parameter::EqHighGain, 0.9);  // -2dB default
        controller.SetParameter(Parameter::EqLowpassEnabled, 1.0);
        controller.SetParameter(Parameter::EqCutoff, 0.75);   // ~8kHz default
        controller.SetParameter(Parameter::EqCrossSeed, 0.5);

        controller.SetParameter(Parameter::SeedTap, 0.5);
        controller.SetParameter(Parameter::SeedDiffusion, 0.5);
        controller.SetParameter(Parameter::SeedDelay, 0.5);
        controller.SetParameter(Parameter::SeedPostDiffusion, 0.5);
    }
};
