// BinauralProcessorPosition: HRIR selection from azimuth/elevation, front ILD guard, ITD correction.
// Realtime-safety: no allocation/locks in updateHRIRFromPosition()

#include "BinauralProcessor.h"
#include <cmath>

void BinauralProcessor::updateHRIRFromPosition(int numSamplesForSmoothing)
{
    if (!hrtfDatabase->isLoaded())
        return;

    const int qualityMode = getEffectiveQualityMode(hrtfQuality.load());
    applyQualityHrirCap(qualityMode);

    // Get smoothed position values (advanced per block in processBlock)
    // The shortest-arc scalar ramp can temporarily extend past +/-180.
    // Normalize before either the spherical path or the HRIR lookup uses it.
    float azimuth = std::remainder(azimuthSmoother.getCurrentValue(), 360.0f);
    float elevation = elevationSmoother.getCurrentValue();
    float widthPercent = sourceWidthSmoother.getCurrentValue();
    float widthFactor = juce::jlimit(0.0f, 1.0f, widthPercent / 100.0f);

    SphericalCoord renderPosition = (positionInterpolation < 1.0f)
                                        ? slerpPosition(previousPosition, targetPosition, positionInterpolation)
                                        : SphericalCoord{azimuth, elevation};

    float effectiveAz = renderPosition.azimuth;
    float effectiveEl = renderPosition.elevation;

    // Distance-aware azimuth smoothing to reduce center-jitter artifacts.
    {
        float distance = distanceSmoother.getCurrentValue();
        const int smoothingSamples = (numSamplesForSmoothing > 0) ? numSamplesForSmoothing : blockSize;
        float azAlpha = calculateDistanceAwareAzimuthAlpha(distance, smoothingSamples);

        if (!azimuthFilterInitialized)
        {
            azimuthFiltered = effectiveAz;
            azimuthFilterInitialized = true;
        }
        else
        {
            float delta = effectiveAz - azimuthFiltered;
            if (delta > 180.0f)
                delta -= 360.0f;
            else if (delta < -180.0f)
                delta += 360.0f;
            azimuthFiltered += delta * azAlpha;
            if (azimuthFiltered > 180.0f)
                azimuthFiltered -= 360.0f;
            else if (azimuthFiltered < -180.0f)
                azimuthFiltered += 360.0f;
        }

        effectiveAz = azimuthFiltered;
    }
    effectiveRenderPosition = {effectiveAz, effectiveEl};

    // Performance takes the nearest measured HRIR; Balanced and Quality let
    // libmysofa interpolate between neighbouring measurements.
    const bool interpolate = qualityMode > 0 && !isBypassedForTest(TestBypass::HrirInterpolation);

    // Only fetch NEW HRIR when position changes significantly
    // Thresholds are tighter for higher-quality interpolation to reduce stepping
    float azDiff = std::abs(effectiveAz - lastIRUpdateAz);
    float elDiff = std::abs(effectiveEl - lastIRUpdateEl);
    float widthDiff = std::abs(widthFactor - lastIRUpdateWidth);
    const bool qualityDiff = (qualityMode != lastIRUpdateQualityMode);
    const bool hrirLengthDiff = (hrirLength != lastIRUpdateHrirLength);
    const bool interpolationDiff = (static_cast<int>(interpolate) != lastIRUpdateInterpolation);
    const bool structuralDiff = qualityDiff || hrirLengthDiff || interpolationDiff;

    // Handle wraparound for azimuth
    if (azDiff > 180.0f)
        azDiff = 360.0f - azDiff;

    float updateThreshold = 3.0f;
    float widthThreshold = 0.02f;  // 2% width change
    if (interpolate && qualityMode == 1)
    {
        updateThreshold = 2.0f;
        widthThreshold = 0.015f;
    }
    else if (interpolate)
    {
        updateThreshold = 1.0f;
        widthThreshold = 0.01f;
    }
    // Looser thresholds in lower quality modes to reduce CPU spikes from HRIR churn.
    if (qualityMode <= 0)
    {
        updateThreshold *= 1.5f;
        widthThreshold *= 1.5f;
    }
    else if (qualityMode == 1)
    {
        updateThreshold *= 1.2f;
        widthThreshold *= 1.2f;
    }

    // Increase thresholds at close distances to reduce rapid HRIR updates
    float distForThreshold = distanceSmoother.getCurrentValue();
    float distNorm = juce::jlimit(0.0f, 1.0f, (distForThreshold - 1.0f) / 2.25f);
    float distanceScale = 1.0f + (1.0f - distNorm) * 1.5f;  // up to 2.5x at 1m
    updateThreshold *= distanceScale;
    widthThreshold *= distanceScale;

    bool hrirChanged = false;
    bool canUpdateHRIR = structuralDiff || (hrirUpdateCooldownSamples <= 0);
    if (canUpdateHRIR &&
        (azDiff > updateThreshold || elDiff > updateThreshold || widthDiff > widthThreshold || structuralDiff))
    {
        // Position changed significantly - get new HRIR. Without the libmysofa
        // lookup the nearest measured HRIR stands in.
        float sofaDelayL = 0.0f;
        float sofaDelayR = 0.0f;
        const bool usedSofa =
            hrtfDatabase->sampleSofaHRIR(effectiveAz, effectiveEl, interpolate, targetHRIR, sofaDelayL, sofaDelayR);
        if (!usedSofa)
            targetHRIR = hrtfDatabase->getHRIR(effectiveAz, effectiveEl);

        applyFrontMedianPlaneIldGuard(targetHRIR, effectiveAz, effectiveEl, interpolate);

        // Diffuse-field EQ (see BinauralProcessorDiffuse.cpp), applied before
        // the width blend: applySourceWidth() builds its spread target from the
        // corrected HRIR, so both blend components carry the same correction
        // and the width stage's per-ear energy restore acts on the spectra
        // that reach the convolver.
        applyDiffuseFieldEQ(targetHRIR);

        // Source width: blend the focused HRIR toward its decorrelated copy.
        if (widthFactor > 0.01f && !isBypassedForTest(TestBypass::Width))
            applySourceWidth(targetHRIR, widthFactor);

        // Update low-band ITD targets based on interpolated direction. The
        // smoothed precomputed ITD field avoids estimator jitter on
        // interpolated HRIRs.
        float itdSeconds = 0.0f;
        float measuredItdSeconds = 0.0f;
        bool measuredItdValid = false;
        if (itdsPrecomputed)
        {
            itdSeconds = interpolateITD(effectiveAz, effectiveEl);
        }
        else
        {
            measuredItdSeconds = estimateITDSamples(targetHRIR) / static_cast<float>(sampleRate);
            measuredItdValid = true;
            itdSeconds = measuredItdSeconds;
        }

        // Near the front median plane, pull the ITD toward the geometric
        // sin(azimuth) model.
        {
            constexpr float frontWidthDegrees = 12.0f;
            constexpr float frontFullStrengthDegrees = 5.0f;
            constexpr float elevationFullStrengthDegrees = 30.0f;
            constexpr float elevationFadeEndDegrees = 60.0f;

            const float absAz = std::abs(effectiveAz);
            const float absEl = std::abs(effectiveEl);
            if (absAz < frontWidthDegrees && absEl < elevationFadeEndDegrees)
            {
                float azStrength = 1.0f;
                if (absAz > frontFullStrengthDegrees)
                    azStrength =
                        1.0f - ((absAz - frontFullStrengthDegrees) / (frontWidthDegrees - frontFullStrengthDegrees));

                float elStrength = 1.0f;
                if (absEl > elevationFullStrengthDegrees)
                    elStrength = 1.0f - ((absEl - elevationFullStrengthDegrees) /
                                         (elevationFadeEndDegrees - elevationFullStrengthDegrees));

                const float strength = juce::jlimit(0.0f, 1.0f, azStrength * elStrength);
                const float geometricItd =
                    std::sin(effectiveAz * juce::MathConstants<float>::pi / 180.0f) * MAX_ITD_SECONDS;
                itdSeconds += (geometricItd - itdSeconds) * strength;
            }
        }

        if (usedSofa)
        {
            const float delayLSamples = sofaDelayL * static_cast<float>(sampleRate);
            const float delayRSamples = sofaDelayR * static_cast<float>(sampleRate);
            if (delayLSamples > 0.0001f)
                applyFractionalDelay(targetHRIR.leftEar.data(), hrirLength, delayLSamples);
            if (delayRSamples > 0.0001f)
                applyFractionalDelay(targetHRIR.rightEar.data(), hrirLength, delayRSamples);

            if (itdsPrecomputed)
            {
                if (!measuredItdValid)
                {
                    measuredItdSeconds = estimateITDSamples(targetHRIR) / static_cast<float>(sampleRate);
                    measuredItdValid = true;
                }

                float deltaSamples = (itdSeconds - measuredItdSeconds) * static_cast<float>(sampleRate);
                const float maxItdSamples = MAX_ITD_SECONDS * static_cast<float>(sampleRate);
                deltaSamples = juce::jlimit(-maxItdSamples, maxItdSamples, deltaSamples);

                if (std::abs(deltaSamples) > 1.0f)
                {
                    if (deltaSamples > 0.0f)
                        applyFractionalDelay(targetHRIR.leftEar.data(), hrirLength, deltaSamples);
                    else
                        applyFractionalDelay(targetHRIR.rightEar.data(), hrirLength, -deltaSamples);
                }
            }
        }

        currentItdSeconds = itdSeconds;
        updateLowBandITDTargets(currentItdSeconds);

        if (diagnosticsEnabled)
        {
            diagnostics.usingSofa = usedSofa;
            diagnostics.sofaInterpolated = usedSofa && interpolate;
            diagnostics.sofaDelaySecondsL = usedSofa ? sofaDelayL : 0.0f;
            diagnostics.sofaDelaySecondsR = usedSofa ? sofaDelayR : 0.0f;
        }

        lastIRUpdateAz = effectiveAz;
        lastIRUpdateEl = effectiveEl;
        lastIRUpdateWidth = widthFactor;
        lastIRUpdateQualityMode = qualityMode;
        lastIRUpdateHrirLength = hrirLength;
        lastIRUpdateInterpolation = static_cast<int>(interpolate);
        hrirChanged = true;

        // Rate-limit HRIR updates more aggressively at close distances.
        // 1.0m -> ~38ms cooldown, 3.25m+ -> ~8ms cooldown.
        float dist = distanceSmoother.getCurrentValue();
        float cooldownDistNorm = juce::jlimit(0.0f, 1.0f, (dist - 1.0f) / 2.25f);
        float cooldownMs = 8.0f + 30.0f * (1.0f - cooldownDistNorm);
        if (qualityMode <= 0)
            cooldownMs *= 1.6f;
        else if (qualityMode == 1)
            cooldownMs *= 1.25f;
        hrirUpdateCooldownSamples = static_cast<int>(cooldownMs * 0.001f * sampleRate);

        // Suppress early reflections briefly after HRIR changes (reduces modulation artifacts).
        float suppressMs = 20.0f + 40.0f * (1.0f - cooldownDistNorm);  // 60ms @ 1m, 20ms @ 3.25m+
        erSuppressionSamplesMax = static_cast<int>(suppressMs * 0.001f * sampleRate);
        erSuppressionSamples = erSuppressionSamplesMax;
    }

    // Always call setImpulseResponse to manage transition state.
    // FFT work is gated by the hrirChanged flag (only executes when position threshold is exceeded).
    leftEarConvA->setImpulseResponse(targetHRIR.leftEar.data(), hrirLength, hrirChanged);
    rightEarConvA->setImpulseResponse(targetHRIR.rightEar.data(), hrirLength, hrirChanged);
    sideLeftEarConvA->setImpulseResponse(targetHRIR.leftEar.data(), hrirLength, hrirChanged);
    sideRightEarConvA->setImpulseResponse(targetHRIR.rightEar.data(), hrirLength, hrirChanged);
}

// Inside the front sector, blends the HRIR toward a left/right-symmetric
// version (averaged with the HRIR sampled at -azimuth, ears swapped) and
// restores its energy, so measurement asymmetry adds no ILD near 0 degrees.
void BinauralProcessor::applyFrontMedianPlaneIldGuard(HRIRData& hrir, float azimuth, float elevation, bool interpolate)
{
    constexpr float frontWidthDegrees = 12.0f;
    constexpr float frontFullStrengthDegrees = 5.0f;
    constexpr float elevationFullStrengthDegrees = 30.0f;
    constexpr float elevationFadeEndDegrees = 60.0f;

    const float absAz = std::abs(azimuth);
    const float absEl = std::abs(elevation);
    if (absAz >= frontWidthDegrees || absEl >= elevationFadeEndDegrees || hrirLength <= 0 || !hrtfDatabase)
        return;

    float azStrength = 1.0f;
    if (absAz > frontFullStrengthDegrees)
        azStrength = 1.0f - ((absAz - frontFullStrengthDegrees) / (frontWidthDegrees - frontFullStrengthDegrees));

    float elStrength = 1.0f;
    if (absEl > elevationFullStrengthDegrees)
        elStrength =
            1.0f - ((absEl - elevationFullStrengthDegrees) / (elevationFadeEndDegrees - elevationFullStrengthDegrees));

    const float strength = juce::jlimit(0.0f, 1.0f, azStrength * elStrength);
    if (strength <= 0.0f)
        return;

    HRIRData mirrored;
    float mirrorDelayL = 0.0f;
    float mirrorDelayR = 0.0f;
    if (!hrtfDatabase->sampleSofaHRIR(-azimuth, elevation, interpolate, mirrored, mirrorDelayL, mirrorDelayR))
        return;
    juce::ignoreUnused(mirrorDelayL, mirrorDelayR);

    double originalEnergy = 0.0;
    double blendedEnergy = 0.0;

    for (int i = 0; i < hrirLength; ++i)
    {
        const size_t idx = static_cast<size_t>(i);
        const float currentL = hrir.leftEar[idx];
        const float currentR = hrir.rightEar[idx];
        const float symmetricL = 0.5f * (currentL + mirrored.rightEar[idx]);
        const float symmetricR = 0.5f * (currentR + mirrored.leftEar[idx]);
        const float blendedL = currentL + (symmetricL - currentL) * strength;
        const float blendedR = currentR + (symmetricR - currentR) * strength;

        originalEnergy += static_cast<double>(currentL) * static_cast<double>(currentL) +
                          static_cast<double>(currentR) * static_cast<double>(currentR);
        blendedEnergy += static_cast<double>(blendedL) * static_cast<double>(blendedL) +
                         static_cast<double>(blendedR) * static_cast<double>(blendedR);

        hrir.leftEar[idx] = blendedL;
        hrir.rightEar[idx] = blendedR;
    }

    if (originalEnergy > 1.0e-16 && blendedEnergy > 1.0e-16)
    {
        const float gain = std::sqrt(static_cast<float>(originalEnergy / blendedEnergy));
        for (int i = 0; i < hrirLength; ++i)
        {
            hrir.leftEar[static_cast<size_t>(i)] *= gain;
            hrir.rightEar[static_cast<size_t>(i)] *= gain;
        }
    }
}
