// BinauralProcessorProcess: Audio-rate HRTF convolution processing pipeline.
// Realtime-safety: No allocation/locks in processBlock(); pre-allocated buffers

#include "BinauralProcessor.h"

//==============================================================================
// Main processing
//==============================================================================
void BinauralProcessor::processBlock(juce::AudioBuffer<float>& buffer)
{
    // If HRTF not loaded, can't process - return early
    if (!hrtfDatabase->isLoaded())
        return;

    // Get current enable state and update wet mix target accordingly
    // This allows smooth crossfade when toggling instead of hard cut
    bool enabled = isEnabled.load();
    if (!enabled)
    {
        // When disabled, fade to 0% wet (full dry)
        wetMixSmoother.setTargetValue(0.0f);
    }
    else
    {
        // When enabled, use the actual wet mix setting
        wetMixSmoother.setTargetValue(wetMix.load());
    }

    const int numSamples = buffer.getNumSamples();
    const int numInputChannels = buffer.getNumChannels();

    if (hrirUpdateCooldownSamples > 0)
        hrirUpdateCooldownSamples = juce::jmax(0, hrirUpdateCooldownSamples - numSamples);
    if (erSuppressionSamples > 0)
        erSuppressionSamples = juce::jmax(0, erSuppressionSamples - numSamples);

    if (numSamples > monoBuffer.getNumSamples() || numSamples > wetBuffer.getNumSamples() ||
        numSamples > convBuffer.getNumSamples() || numSamples > sideBuffer.getNumSamples() ||
        numSamples > sideConvBuffer.getNumSamples() || numSamples > erInputBuffer.getNumSamples() ||
        numSamples > erAccumBuffer.getNumSamples() || numSamples > tempBuffer.getNumSamples())
    {
        jassertfalse;  // buffer sizes should be sized to maxBlockSize in prepare()
        return;
    }

    // The mix loop advances the ramp once per rendered sample. Advancing it
    // here as well halves a disable fade and introduces a gain step at each
    // callback. Skip the wet path only after the full ramp has reached dry.
    if (!wetMixSmoother.isSmoothing() && wetMixSmoother.getCurrentValue() == 0.0f)
        return;

    // Snapshot original channels for dry mix preservation.
    const float* dryLeftIn = buffer.getReadPointer(0);
    const float* dryRightIn = (numInputChannels > 1) ? buffer.getReadPointer(1) : dryLeftIn;

    // Create mono sum of input
    monoBuffer.clear();
    const float invNumChannels = 1.0f / static_cast<float>(numInputChannels);
    for (int ch = 0; ch < numInputChannels; ++ch)
    {
        monoBuffer.addFrom(0, 0, buffer, ch, 0, numSamples, invNumChannels);
    }

    // Keep the scalar azimuth ramp on the same angular branch as its new
    // target. Sub-degree motion does not invoke SLERP, but still crosses the
    // rear seam: +179.8 -> -179.8 must ramp through 180, never through zero.
    // Rebase only on a target seam crossing so a repeated target retains its
    // existing ramp progress. This runs exclusively on the audio thread.
    const float azimuthTarget = currentAzimuth.load();
    const float targetDelta = azimuthTarget - azimuthSmoother.getTargetValue();
    if (targetDelta > 180.0f)
        azimuthSmoother.setCurrentAndTargetValue(azimuthSmoother.getCurrentValue() + 360.0f);
    else if (targetDelta < -180.0f)
        azimuthSmoother.setCurrentAndTargetValue(azimuthSmoother.getCurrentValue() - 360.0f);
    azimuthSmoother.setTargetValue(azimuthTarget);

    // Update smoothed parameters (wet mix is already set based on enable state above)
    elevationSmoother.setTargetValue(currentElevation.load());
    distanceSmoother.setTargetValue(currentDistance.load());
    // wetMixSmoother target already set above based on enable state - don't override!
    sourceWidthSmoother.setTargetValue(sourceWidth.load());

    // Advance smoothers by block size to avoid block-rate lag.
    azimuthSmoother.skip(numSamples);
    elevationSmoother.skip(numSamples);
    sourceWidthSmoother.skip(numSamples);
    advancePositionInterpolation(numSamples);

    // Update HRIR from current smoothed position (gated by thresholds/cooldown)
    updateHRIRFromPosition(numSamples);

    // === CROSSOVER-BASED PROCESSING ===
    // Key insight: Only apply HRTF to frequencies ABOVE the crossover (default ~150Hz)
    // Bass passes through unchanged - provides stable anchor during transitions
    // This dramatically reduces choppiness because:
    // 1. Bass is less affected by HRTF spectral changes
    // 2. HRTF mainly affects mid/high frequencies perceptually
    // 3. Stable bass "grounds" the sound during filter transitions

    wetBuffer.clear();

    // Split into low-pass (bass) and high-pass (for HRTF processing)
    lowFreqBuffer.copyFrom(0, 0, monoBuffer, 0, 0, numSamples);
    highFreqBuffer.copyFrom(0, 0, monoBuffer, 0, 0, numSamples);

    // Apply crossover filters (150Hz default, user-adjustable).
    // CRITICAL: wrap only the live region. The buffers are allocated at the
    // prepared maximum (16384 via PluginProcessor's safeBlockSize), so wrapping
    // the whole buffer makes the IIR filters also process the stale tail every
    // block - corrupting their state and emitting a click at each block start.
    juce::dsp::AudioBlock<float> lowBlock =
        juce::dsp::AudioBlock<float>(lowFreqBuffer).getSubBlock(0, (size_t)numSamples);
    juce::dsp::AudioBlock<float> highBlock =
        juce::dsp::AudioBlock<float>(highFreqBuffer).getSubBlock(0, (size_t)numSamples);
    juce::dsp::ProcessContextReplacing<float> lowContext(lowBlock);
    juce::dsp::ProcessContextReplacing<float> highContext(highBlock);
    lowMidLowPass.process(lowContext);    // Extract bass (<150Hz)
    lowMidHighPass.process(highContext);  // Extract mids+highs (>150Hz)

    // Align the bass anchor with the HRIR's group delay at the crossover.
    // The convolver itself adds no delay, regardless of callback size.
    updateLowBandAlignment();

    // Apply low-band ITD delay to the bass anchor (creates directional timing cues)
    auto* lowMono = lowFreqBuffer.getReadPointer(0);
    auto* lowL = lowFreqBuffer.getWritePointer(0);
    auto* lowR = tempBuffer.getWritePointer(0);
    applyLowBandITD(lowMono, lowL, lowR, numSamples);

    // Only process high frequencies through HRTF convolution
    // ConvolutionEngine caches each transfer function. Its FFT tail advances
    // on fixed 64-sample hops; IR transitions temporarily render both filters.
    auto* highData = highFreqBuffer.getReadPointer(0);
    auto* leftConv = convBuffer.getWritePointer(0);
    auto* rightConv = convBuffer.getWritePointer(1);

    leftEarConvA->process(highData, leftConv, numSamples);
    rightEarConvA->process(highData, rightConv, numSamples);

    sideConvBuffer.clear();
    // Side content must always be rendered for stereo inputs: the width parameter
    // shapes the blend later, but a pure-side signal (L = -R) has zero mid energy,
    // so skipping side convolution on any width setting silences the wet path.
    if (numInputChannels > 1)
    {
        auto* sideData = sideBuffer.getWritePointer(0);
        const auto* dryL = dryLeftIn;
        const auto* dryR = dryRightIn;
        for (int i = 0; i < numSamples; ++i)
            sideData[i] = 0.5f * (dryL[i] - dryR[i]);

        sideLeftEarConvA->process(sideData, sideConvBuffer.getWritePointer(0), numSamples);
        sideRightEarConvA->process(sideData, sideConvBuffer.getWritePointer(1), numSamples);
    }

    // Front/back cue is applied only to the HRTF-processed high band.
    const float azimuth = effectiveRenderPosition.azimuth;
    const float frontBackFactor = std::cos(azimuth * juce::MathConstants<float>::pi / 180.0f);
    const float highBandCueGain = (std::abs(frontBackFactor) > 0.1f) ? (1.0f + frontBackFactor * 0.04f) : 1.0f;

    // Combine: HRTF-processed highs + ITD-delayed bass anchor
    auto* bassL = lowFreqBuffer.getReadPointer(0);
    auto* bassR = tempBuffer.getReadPointer(0);
    auto* leftOut = wetBuffer.getWritePointer(0);
    auto* rightOut = wetBuffer.getWritePointer(1);
    const auto* sideLeft = sideConvBuffer.getReadPointer(0);
    const auto* sideRight = sideConvBuffer.getReadPointer(1);

    for (int i = 0; i < numSamples; ++i)
    {
        leftOut[i] = bassL[i] + highBandCueGain * (leftConv[i] + sideLeft[i]);
        rightOut[i] = bassR[i] + highBandCueGain * (rightConv[i] - sideRight[i]);
    }

    // Apply distance model with per-sample smoothing
    auto* leftWet = wetBuffer.getWritePointer(0);
    auto* rightWet = wetBuffer.getWritePointer(1);

    for (int i = 0; i < numSamples; ++i)
    {
        float distTarget = distanceSmoother.getNextValue();
        float distNorm = juce::jlimit(0.0f, 1.0f, (distTarget - 1.0f) / 2.25f);
        float distAlpha = 0.05f + 0.25f * distNorm;  // stronger smoothing near 1m

        if (!distanceFilterInitialized)
        {
            distanceFiltered = distTarget;
            distanceFilterInitialized = true;
        }
        else
        {
            distanceFiltered += (distTarget - distanceFiltered) * distAlpha;
        }

        float attenuation = calculateDistanceAttenuation(distanceFiltered);
        leftWet[i] *= attenuation;
        rightWet[i] *= attenuation;
    }

    float currentDist = distanceFiltered;

    // Apply frequency-dependent air absorption
    if (!isBypassedForTest(TestBypass::AirAbsorption))
    {
        // Higher frequencies absorbed more: exponential relationship with distance

        // A TPT state-variable filter at or above Nyquist is unstable: below
        // 44.1 kHz, unclamped 20 kHz / 16 kHz close-range cutoffs would blow
        // the whole spatial path up to NaN (heard as permanent silence).
        const float maxAirCutoff = getMaxAirAbsorptionCutoff();

        // Low band cutoff: 20kHz (close) to 8kHz (far) - gentle rolloff
        float lowCutoff = juce::jmin(maxAirCutoff, juce::jmap(currentDist, 0.1f, 10.0f, 20000.0f, 8000.0f));
        airAbsorptionLowL.setCutoffFrequency(lowCutoff);
        airAbsorptionLowR.setCutoffFrequency(lowCutoff);

        // High band cutoff: 16kHz (close) to 3kHz (far) - stronger rolloff
        float highCutoff = juce::jmin(maxAirCutoff, juce::jmap(currentDist, 0.1f, 10.0f, 16000.0f, 3000.0f));
        airAbsorptionHighL.setCutoffFrequency(highCutoff);
        airAbsorptionHighR.setCutoffFrequency(highCutoff);

        // Process multi-band air absorption (cascade both filters)
        juce::dsp::AudioBlock<float> leftBlock(&leftWet, 1, static_cast<size_t>(numSamples));
        juce::dsp::AudioBlock<float> rightBlock(&rightWet, 1, static_cast<size_t>(numSamples));
        juce::dsp::ProcessContextReplacing<float> leftContext(leftBlock);
        juce::dsp::ProcessContextReplacing<float> rightContext(rightBlock);

        airAbsorptionLowL.process(leftContext);
        airAbsorptionLowR.process(rightContext);
        airAbsorptionHighL.process(leftContext);
        airAbsorptionHighR.process(rightContext);
    }

    // Apply Near-Field DVF (Spagnol et al. 2017): bass boost and ILD
    // enhancement for sources closer than 1m, from precomputed lookup tables.
    if (!isBypassedForTest(TestBypass::NearFieldDvf))
    {
        updateNearFieldDVF(currentDist);

        // Process DVF per-sample using smoothed coefficients (avoids zipper/clicks)
        for (int i = 0; i < numSamples; ++i)
        {
            for (int c = 0; c < 3; ++c)
            {
                dvfCoeffsL[c] += (dvfCoeffsTargetL[c] - dvfCoeffsL[c]) * dvfCoeffSmoothAlpha;
                dvfCoeffsR[c] += (dvfCoeffsTargetR[c] - dvfCoeffsR[c]) * dvfCoeffSmoothAlpha;
            }
            nearFieldCompensationGain +=
                (nearFieldCompensationTarget - nearFieldCompensationGain) * nearFieldGainSmoothAlpha;

            leftWet[i] = nearFieldCompensationGain * processDVFSample(leftWet[i], dvfCoeffsL, dvfStateL);
            rightWet[i] = nearFieldCompensationGain * processDVFSample(rightWet[i], dvfCoeffsR, dvfStateR);
        }
    }

    // Apply early reflections (adds room ambience based on distance)
    // Optimization: disable at very far distances (>8m) where reflections inaudible
    // (Griesinger 1997: early reflection perceptual threshold ~80ms, ~27m at speed of sound)
    const bool erEnabled = !isBypassedForTest(TestBypass::EarlyReflections) && (currentDist < 8.0f);
    if (erEnabled)
    {
        // wetBuffer is sized to the prepared maximum. Passing it whole would
        // advance the reflection delay lines by that size every callback,
        // feeding them silence whenever the host delivers a shorter block.
        juce::AudioBuffer<float> liveWet(wetBuffer.getArrayOfWritePointers(), wetBuffer.getNumChannels(), numSamples);
        processEarlyReflections(liveWet, currentDist);
    }

    // Apply near-field output safety trim.
    // Near-field filters can introduce sizable gain, so we trim only in close-distance regimes.
    float nearFieldPreSafetyPeak = 0.0f;
    float nearFieldPostSafetyPeak = 0.0f;
    if (!isBypassedForTest(TestBypass::NearFieldSafety))
    {
        const float nearFieldSafetyBlend = juce::jlimit(0.0f, 1.0f, (1.0f - currentDist) / 0.9f);
        for (int i = 0; i < numSamples; ++i)
        {
            const float peak = std::max(std::abs(leftWet[i]), std::abs(rightWet[i]));
            nearFieldPreSafetyPeak = std::max(nearFieldPreSafetyPeak, peak);
            const float envAlpha =
                (peak > nearFieldSafetyEnv) ? nearFieldSafetyAttackAlpha : nearFieldSafetyReleaseAlpha;
            nearFieldSafetyEnv += (peak - nearFieldSafetyEnv) * envAlpha;

            float targetSafetyGain = 1.0f;
            if (nearFieldSafetyEnv > kNearFieldSafetyThreshold)
                targetSafetyGain = kNearFieldSafetyThreshold / juce::jmax(nearFieldSafetyEnv, 1.0e-6f);

            targetSafetyGain = juce::jlimit(kNearFieldSafetyMinGain, 1.0f, targetSafetyGain);
            targetSafetyGain = 1.0f - nearFieldSafetyBlend * (1.0f - targetSafetyGain);

            const float gainAlpha =
                (targetSafetyGain < nearFieldSafetyGain) ? nearFieldSafetyAttackAlpha : nearFieldSafetyReleaseAlpha;
            nearFieldSafetyGain += (targetSafetyGain - nearFieldSafetyGain) * gainAlpha;

            leftWet[i] *= nearFieldSafetyGain;
            rightWet[i] *= nearFieldSafetyGain;
            nearFieldPostSafetyPeak =
                std::max(nearFieldPostSafetyPeak, std::max(std::abs(leftWet[i]), std::abs(rightWet[i])));
        }
    }
    else
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float peak = std::max(std::abs(leftWet[i]), std::abs(rightWet[i]));
            nearFieldPreSafetyPeak = std::max(nearFieldPreSafetyPeak, peak);
            nearFieldPostSafetyPeak = std::max(nearFieldPostSafetyPeak, peak);
        }
        nearFieldSafetyEnv = 0.0f;
        nearFieldSafetyGain = 1.0f;
    }

    if (diagnosticsEnabled)
    {
        diagnostics.nearFieldSafetyGain = nearFieldSafetyGain;
        diagnostics.nearFieldPreSafetyPeak = nearFieldPreSafetyPeak;
        diagnostics.nearFieldPostSafetyPeak = nearFieldPostSafetyPeak;
    }

    // Apply wet/dry mix with a shared per-sample ramp (avoids channel mismatch)
    auto* dryMono = monoBuffer.getReadPointer(0);
    auto* wetMixRamp = tempBuffer.getWritePointer(0);
    for (int i = 0; i < numSamples; ++i)
        wetMixRamp[i] = wetMixSmoother.getNextValue();

    const int outChannels = buffer.getNumChannels();
    if (outChannels < 2)
    {
        auto* outputChannel = buffer.getWritePointer(0);
        auto* wetL = wetBuffer.getReadPointer(0);
        auto* wetR = wetBuffer.getReadPointer(1);
        for (int i = 0; i < numSamples; ++i)
        {
            float wet = wetMixRamp[i];
            float dry = 1.0f - wet;
            float wetMono = 0.5f * (wetL[i] + wetR[i]);
            outputChannel[i] = wet * wetMono + dry * dryMono[i];
        }
    }
    else
    {
        auto* outputL = buffer.getWritePointer(0);
        auto* outputR = buffer.getWritePointer(1);
        auto* wetL = wetBuffer.getReadPointer(0);
        auto* wetR = wetBuffer.getReadPointer(1);

        for (int i = 0; i < numSamples; ++i)
        {
            const float wet = wetMixRamp[i];
            const float dry = 1.0f - wet;

            const float dryL = dryLeftIn[i];
            const float dryR = dryRightIn[i];

            outputL[i] = wet * wetL[i] + dry * dryL;
            outputR[i] = wet * wetR[i] + dry * dryR;
        }
    }

    // Collect diagnostics at end of processing
    if (diagnosticsEnabled)
    {
        // Calculate band levels (RMS to dB)
        auto calcRmsDb = [numSamples](const float* data)
        {
            float sum = 0.0f;
            for (int i = 0; i < numSamples; ++i)
                sum += data[i] * data[i];
            float rms = std::sqrt(sum / static_cast<float>(numSamples));
            return (rms > 1e-10f) ? 20.0f * std::log10(rms) : -100.0f;
        };

        diagnostics.lowBandDb = calcRmsDb(lowFreqBuffer.getReadPointer(0));
        diagnostics.highBandDb = calcRmsDb(highFreqBuffer.getReadPointer(0));

        // SLERP state
        diagnostics.slerpT = positionInterpolation;
        diagnostics.slerpAzimuth = effectiveRenderPosition.azimuth;
        diagnostics.slerpElevation = effectiveRenderPosition.elevation;
    }
}

float BinauralProcessor::calculateDistanceAttenuation(float distance)
{
    // Inverse-clamped style attenuation (OpenAL-style) with gentler rolloff than pure 1/d.
    // This preserves spatial depth while avoiding overly rapid level drop during normal moves.
    const float referenceDistance = 1.0f;  // Unity gain at 1m
    const float minDistance = 0.1f;        // Safety clamp for near-field
    const float maxDistance = 7.5f;        // Distances beyond this stop attenuating further
    const float rolloffFactor = 0.55f;     // Gentler than physical 1.0 rolloff for musical UX
    const float minGain = 0.2f;            // Floor at about -14 dB

    const float d = juce::jlimit(minDistance, maxDistance, distance);
    if (d <= referenceDistance)
        return 1.0f;

    const float attenuation = referenceDistance / (referenceDistance + rolloffFactor * (d - referenceDistance));
    return juce::jlimit(minGain, 1.0f, attenuation);
}
