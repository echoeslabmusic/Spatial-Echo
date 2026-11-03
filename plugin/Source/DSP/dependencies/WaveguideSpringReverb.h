// WaveguideSpringReverbNetwork: Spring engine front end. Owns two prepared triple-spring tanks and crossfades between
//   them when a Size change would otherwise retune the delay geometry of the live tank.
// Realtime-safety: No allocation in process()
// Based on: Välimäki, Parker, Abel, "Parametric Spring Reverberation Effect" (2010); Parker, "Efficient Dispersion
//   Generation Structures" (2011); Bilbao, Parker, "A Virtual Model of Spring Reverberation" (2010); Schlecht/Habets,
//   "Time-Varying Feedback Matrices in FDNs" (2015)

#pragma once

#include "ImprovedSpringReverb.h"
#include <algorithm>
#include <cmath>
#include <utility>

/**
 * Spring reverb engine with a Size-morph crossfade.
 *
 * Every parameter except Size is applied to the live tank directly. A Size
 * change of at least kSizeMorphThreshold is applied to a copy of the live tank
 * instead, and the output crossfades from the live tank to that copy over
 * kSizeMorphMs, so the delay lengths never jump under audio.
 */
class WaveguideSpringReverbNetwork
{
public:
    WaveguideSpringReverbNetwork() = default;
    ~WaveguideSpringReverbNetwork() = default;

    float processSample(float xn)
    {
        const float primary = activeProc->processSample(xn);
        if (!sizeMorphActive)
            return primary;

        const float morph = morphProc->processSample(xn);
        const float out = primary + (morph - primary) * sizeMorphMix;

        sizeMorphMix = std::min(1.0f, sizeMorphMix + sizeMorphStep);
        if (sizeMorphMix >= 1.0f)
            finishSizeMorph();

        return out;
    }

    // maxBlockSize is the host's prepared maximum callback size. It latches the
    // reduced-cost topology decision once, here, instead of re-deciding it under
    // audio (see TripleSpringReverb::configurePerformanceMode).
    void reset(float sampleRate, int maxBlockSize = 512)
    {
        fs = sampleRate;
        const int preparedBlockSize = std::max(1, maxBlockSize);
        activeProc = &procA;
        morphProc = &procB;

        activeProc->prepare(sampleRate, preparedBlockSize);
        morphProc->prepare(sampleRate, preparedBlockSize);

        sizeMorphActive = false;
        sizeMorphMix = 1.0f;
        sizeMorphStep = 0.0f;
        hasQueuedParams = false;
        paramsInitialized = false;

        // Start from the default spring voicing (TripleSpringReverb::Params).
        // The noise switch and amount follow the global tape-noise controls, so
        // they keep their last value across a re-prepare.
        const bool noiseEnabled = targetParams.noiseEnabled;
        const float noiseAmount = targetParams.noiseAmount;
        targetParams = TripleSpringReverb::Params{};
        targetParams.noiseEnabled = noiseEnabled;
        targetParams.noiseAmount = noiseAmount;
        applyParams(targetParams);
    }

    // Clear recursive audio state without re-preparing or resizing storage.
    // Keep the active processor and applied parameter description so the
    // caller can re-apply the current target without losing configuration.
    void clearAudioState()
    {
        procA.reset();
        procB.reset();
        sizeMorphActive = false;
        sizeMorphMix = 1.0f;
        sizeMorphStep = 0.0f;
        hasQueuedParams = false;
    }

    // Batch parameter update - sets all values and rebuilds once.
    // Use this from the main parameter update path to avoid N×rebuild overhead.
    void SetAllParameters(float size, float decay, float damping, float diffusion, float modRate, float modDepth,
                          float springBalance, float scatterAmount, float preDelay, float earlyLateMix,
                          float outputGain, bool noiseEnabled, float noiseAmount)
    {
        targetParams.size = size;
        targetParams.decay = decay;
        targetParams.damping = damping;
        targetParams.diffusion = diffusion;
        targetParams.modRate = modRate;
        targetParams.modDepth = modDepth;
        targetParams.springBalance = springBalance;
        targetParams.scatterAmount = scatterAmount;
        targetParams.preDelay = preDelay;
        targetParams.earlyLateMix = earlyLateMix;
        targetParams.outputGain = outputGain;
        targetParams.noiseEnabled = noiseEnabled;
        targetParams.noiseAmount = noiseAmount;
        applyParams(targetParams);
    }

    // Individual parameter setters for testing/automation
    void SetSize(float size)
    {
        targetParams.size = size;
        applyParams(targetParams);
    }
    void SetDecay(float decay)
    {
        targetParams.decay = decay;
        applyParams(targetParams);
    }
    void SetDamping(float damping)
    {
        targetParams.damping = damping;
        applyParams(targetParams);
    }
    void SetDiffusion(float diffusion)
    {
        targetParams.diffusion = diffusion;
        applyParams(targetParams);
    }
    void SetModulation(float rate, float depth)
    {
        targetParams.modRate = rate;
        targetParams.modDepth = depth;
        applyParams(targetParams);
    }
    void SetSpringBalance(float balance)
    {
        targetParams.springBalance = balance;
        applyParams(targetParams);
    }
    void SetScatterAmount(float scatterAmount)
    {
        targetParams.scatterAmount = scatterAmount;
        applyParams(targetParams);
    }
    void SetOutputGain(float gain)
    {
        targetParams.outputGain = gain;
        applyParams(targetParams);
    }
    void SetPreDelay(float preDelay)
    {
        targetParams.preDelay = preDelay;
        applyParams(targetParams);
    }
    void SetEarlyLateMix(float mix)
    {
        targetParams.earlyLateMix = mix;
        applyParams(targetParams);
    }
    void SetNoiseEnabled(bool enabled)
    {
        targetParams.noiseEnabled = enabled;
        applyParams(targetParams);
    }
    void SetNoiseAmount(float amount)
    {
        targetParams.noiseAmount = amount;
        applyParams(targetParams);
    }
    // Per-callback refresh of the sample rate. The callback size is accepted
    // but deliberately ignored: the reduced-cost topology is latched in reset()
    // from the prepared maximum block size.
    void SetRealtimeContext(float sampleRate, int /*samplesPerBlock*/)
    {
        fs = sampleRate;
        activeProc->updateRealtimeContext(sampleRate);
        morphProc->updateRealtimeContext(sampleRate);
    }
    bool IsAdaptivePerformanceModeActive() const noexcept { return activeProc->isPerformanceModeEnabled(); }

private:
    static constexpr float kSizeMorphThreshold = 0.015f;
    static constexpr float kParamEpsilon = 1.0e-4f;
    static constexpr float kSizeMorphMs = 60.0f;

    static bool approximatelyEqual(float a, float b, float eps = kParamEpsilon) { return std::abs(a - b) <= eps; }

    static bool paramsDiffer(const TripleSpringReverb::Params& a, const TripleSpringReverb::Params& b)
    {
        return !approximatelyEqual(a.size, b.size) || !approximatelyEqual(a.decay, b.decay) ||
               !approximatelyEqual(a.diffusion, b.diffusion) || !approximatelyEqual(a.damping, b.damping) ||
               !approximatelyEqual(a.modDepth, b.modDepth) || !approximatelyEqual(a.springBalance, b.springBalance) ||
               !approximatelyEqual(a.scatterAmount, b.scatterAmount) || !approximatelyEqual(a.modRate, b.modRate) ||
               !approximatelyEqual(a.outputGain, b.outputGain) || !approximatelyEqual(a.preDelay, b.preDelay) ||
               !approximatelyEqual(a.earlyLateMix, b.earlyLateMix) || (a.noiseEnabled != b.noiseEnabled) ||
               !approximatelyEqual(a.noiseAmount, b.noiseAmount);
    }

    static float sizeDelta(const TripleSpringReverb::Params& a, const TripleSpringReverb::Params& b)
    {
        return std::abs(a.size - b.size);
    }

    void finishSizeMorph()
    {
        std::swap(activeProc, morphProc);
        sizeMorphActive = false;
        sizeMorphMix = 1.0f;
        sizeMorphStep = 0.0f;
        appliedParams = morphTargetParams;

        if (hasQueuedParams)
        {
            const auto queued = queuedParams;
            hasQueuedParams = false;
            applyParams(queued);
        }
    }

    void startSizeMorph(const TripleSpringReverb::Params& target)
    {
        // Duplicate current engine state before structural retune so the
        // crossfade transitions between two coherent reverberant states.
        // copyStateFrom (not `*morphProc = *activeProc`) because this runs on
        // the audio thread and must not allocate - see the note there.
        morphProc->copyStateFrom(*activeProc);
        morphProc->setParams(target);

        morphTargetParams = target;
        sizeMorphActive = true;
        sizeMorphMix = 0.0f;

        const float morphSamples = std::max(1.0f, fs * (kSizeMorphMs * 0.001f));
        sizeMorphStep = 1.0f / morphSamples;
    }

    void applyParams(const TripleSpringReverb::Params& params)
    {
        if (!paramsInitialized)
        {
            activeProc->setParams(params);
            morphProc->copyStateFrom(*activeProc);
            appliedParams = params;
            morphTargetParams = params;
            queuedParams = params;
            paramsInitialized = true;
            return;
        }

        if (sizeMorphActive)
        {
            queuedParams = params;
            hasQueuedParams = true;
            return;
        }

        const bool sizeNeedsMorph = sizeDelta(params, appliedParams) >= kSizeMorphThreshold;
        if (sizeNeedsMorph)
        {
            startSizeMorph(params);
            return;
        }

        // Keep structural delay geometry stable unless movement exceeds
        // threshold; this prevents block-rate micro-retunes while dragging.
        TripleSpringReverb::Params immediate = params;
        immediate.size = appliedParams.size;

        if (!paramsDiffer(immediate, appliedParams))
            return;

        activeProc->setParams(immediate);
        // The standby processor is overwritten from activeProc immediately
        // before every size morph. Copying the complete prepared tank here for
        // ordinary decay/tone/control updates is redundant and can allocate on
        // the audio thread as nested delay storage is assigned.
        appliedParams = immediate;
        morphTargetParams = immediate;
    }

    // The values the caller has requested; the tanks receive them through
    // applyParams. reset() restores the default voicing.
    TripleSpringReverb::Params targetParams{};

    TripleSpringReverb procA;
    TripleSpringReverb procB;
    TripleSpringReverb* activeProc = &procA;
    TripleSpringReverb* morphProc = &procB;

    TripleSpringReverb::Params appliedParams{};
    TripleSpringReverb::Params morphTargetParams{};
    TripleSpringReverb::Params queuedParams{};

    bool paramsInitialized = false;
    bool hasQueuedParams = false;
    bool sizeMorphActive = false;
    float sizeMorphMix = 1.0f;
    float sizeMorphStep = 0.0f;

    float fs = 44100.0f;
};
