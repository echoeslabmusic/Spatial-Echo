// BinauralProcessorLifecycle: prepare(), reset(), processBlock(); main real-time entry points.
// Realtime-safety: no allocation/locks in processBlock()

#include "BinauralProcessor.h"
#include "OouraFFT.h"
#include <algorithm>
#include <cmath>

void BinauralProcessor::prepare(double newSampleRate, int samplesPerBlock, int newNumChannels)
{
    this->sampleRate = newSampleRate;
    this->blockSize = samplesPerBlock;
    this->numChannels = newNumChannels;

    // Size the convolvers for the quality-capped HRIR length; a dataset load
    // (onHrtfDatabaseChanged, below) re-prepares them for its own length.
    applyQualityHrirCap(evaluateAdaptivePerformanceMode() ? 0 : 2);
    const int prepareHrirLength = juce::jmin(hrirLengthRaw, hrirLength);
    leftEarConvA->prepare(newSampleRate, samplesPerBlock, prepareHrirLength);
    rightEarConvA->prepare(newSampleRate, samplesPerBlock, prepareHrirLength);
    sideLeftEarConvA->prepare(newSampleRate, samplesPerBlock, prepareHrirLength);
    sideRightEarConvA->prepare(newSampleRate, samplesPerBlock, prepareHrirLength);

    // Prepare crossover filters
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = newSampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock);
    spec.numChannels = 1;  // Process mono internally

    // Low/High crossover at 150Hz (default; parameter can override)
    lowMidLowPass.prepare(spec);
    lowMidHighPass.prepare(spec);
    lowMidLowPass.setCutoffFrequency(kLowMidCrossover);
    lowMidHighPass.setCutoffFrequency(kLowMidCrossover);
    lowMidLowPass.setType(juce::dsp::LinkwitzRileyFilterType::lowpass);
    lowMidHighPass.setType(juce::dsp::LinkwitzRileyFilterType::highpass);

    // Multi-band air absorption filters
    // Low band: gentle rolloff for mid frequencies
    airAbsorptionLowL.prepare(spec);
    airAbsorptionLowR.prepare(spec);
    airAbsorptionLowL.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    airAbsorptionLowR.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    airAbsorptionLowL.setCutoffFrequency(juce::jmin(16000.0f, getMaxAirAbsorptionCutoff()));
    airAbsorptionLowR.setCutoffFrequency(juce::jmin(16000.0f, getMaxAirAbsorptionCutoff()));

    // High band: stronger rolloff for high frequencies
    airAbsorptionHighL.prepare(spec);
    airAbsorptionHighR.prepare(spec);
    airAbsorptionHighL.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    airAbsorptionHighR.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    airAbsorptionHighL.setCutoffFrequency(juce::jmin(8000.0f, getMaxAirAbsorptionCutoff()));
    airAbsorptionHighR.setCutoffFrequency(juce::jmin(8000.0f, getMaxAirAbsorptionCutoff()));

    // Near-Field DVF (Spagnol et al. 2017): bass boost (proximity effect) for
    // sources closer than 1m
    updateNearFieldDVF(1.0f);  // Initialize at reference distance (1m = no effect)

    // Smooth DVF coefficient changes to avoid zipper/clicks during distance changes
    {
        constexpr double dvfSmoothTime = 0.030;  // 30 ms smoothing
        dvfCoeffSmoothAlpha = static_cast<float>(1.0 - std::exp(-1.0 / (dvfSmoothTime * newSampleRate)));
        dvfCoeffSmoothAlpha = juce::jlimit(0.0f, 1.0f, dvfCoeffSmoothAlpha);
        nearFieldGainSmoothAlpha = dvfCoeffSmoothAlpha;

        // Fast attack / slower release so near-field transients do not clip.
        // MEASURED overshoot of the near-field guard against the deliberately
        // +4 dB drive used by BinauralNearFieldHeadroom (peak reaching the
        // guard 2.416): 1 ms -> 2.12, 500 us -> 2.11, 200 us -> 1.76,
        // 50 us -> 1.20; settled steady-state output is ~1.03 in every case.
        // Shortening the attack trades overshoot against pumping sensitivity
        // (a shorter attack makes the guard duck on sub-millisecond transients
        // and then hold for the 80 ms release), and there is no measurement
        // here that settles which matters more for this plugin, so the attack
        // stays at 1 ms.
        constexpr double safetyAttackTime = 0.001;   // 1 ms
        constexpr double safetyReleaseTime = 0.080;  // 80 ms
        nearFieldSafetyAttackAlpha = static_cast<float>(1.0 - std::exp(-1.0 / (safetyAttackTime * newSampleRate)));
        nearFieldSafetyReleaseAlpha = static_cast<float>(1.0 - std::exp(-1.0 / (safetyReleaseTime * newSampleRate)));
        nearFieldSafetyAttackAlpha = juce::jlimit(0.0f, 1.0f, nearFieldSafetyAttackAlpha);
        nearFieldSafetyReleaseAlpha = juce::jlimit(0.0f, 1.0f, nearFieldSafetyReleaseAlpha);
        nearFieldCompensationGain = 1.0f;
        nearFieldCompensationTarget = 1.0f;
        nearFieldSafetyEnv = 0.0f;
        nearFieldSafetyGain = 1.0f;
    }

    // Initialize early reflection taps
    initEarlyReflections();

    // Early reflection smoothing (per-sample exponential smoothing)
    {
        constexpr double erDelaySmoothTime = 0.020;  // 20ms delay smoothing
        constexpr double erGainSmoothTime = 0.010;   // 10ms gain smoothing
        erDelaySmoothAlpha = static_cast<float>(1.0 - std::exp(-1.0 / (erDelaySmoothTime * newSampleRate)));
        erGainSmoothAlpha = static_cast<float>(1.0 - std::exp(-1.0 / (erGainSmoothTime * newSampleRate)));
        erDelaySmoothAlpha = juce::jlimit(0.0f, 1.0f, erDelaySmoothAlpha);
        erGainSmoothAlpha = juce::jlimit(0.0f, 1.0f, erGainSmoothAlpha);
        std::fill(erDelaySamplesCurrent.begin(), erDelaySamplesCurrent.end(), 0.0f);
        std::fill(erGainLCurrent.begin(), erGainLCurrent.end(), 0.0f);
        std::fill(erGainRCurrent.begin(), erGainRCurrent.end(), 0.0f);
        erSmoothingInitialized = false;
    }

    // Minimal smoothing for automation - near-instant response
    azimuthSmoother.reset(newSampleRate, 0.002);  // 2ms - nearly instant
    elevationSmoother.reset(newSampleRate, 0.002);
    distanceSmoother.reset(newSampleRate, 0.005);  // 5ms
    wetMixSmoother.reset(newSampleRate, 0.050);    // 50ms - long enough for click-free bypass
    sourceWidthSmoother.reset(8);
    sourceWidthSmoother.setCurrentAndTargetValue(sourceWidth.load());

    // Allocate internal buffers for crossover processing
    monoBuffer.setSize(1, samplesPerBlock);
    lowFreqBuffer.setSize(1, samplesPerBlock);
    highFreqBuffer.setSize(1, samplesPerBlock);
    tempBuffer.setSize(1, samplesPerBlock);
    wetBuffer.setSize(2, samplesPerBlock);
    convBuffer.setSize(2, samplesPerBlock);
    sideBuffer.setSize(1, samplesPerBlock);
    sideConvBuffer.setSize(2, samplesPerBlock);
    erInputBuffer.setSize(2, samplesPerBlock);
    erAccumBuffer.setSize(2, samplesPerBlock);

    // Low-band ITD delay buffer (mono input, two delayed outputs).
    // Sized for the ITD plus the largest crossover alignment delay
    // (kMaxLowBandAlignSamples), so applyLowBandITD can never read past the
    // buffer even at the maximum alignment.
    const int maxItdSamples = static_cast<int>(std::ceil(MAX_ITD_SECONDS * newSampleRate)) + 4;
    lowBandDelayBufferSize = maxItdSamples + kMaxLowBandAlignSamples + samplesPerBlock + 4;
    lowBandDelayBuffer.assign(static_cast<size_t>(lowBandDelayBufferSize), 0.0f);
    lowBandDelayWritePos = 0;
    lowBandDelaySmootherL.reset(newSampleRate, 0.005);
    lowBandDelaySmootherR.reset(newSampleRate, 0.005);
    lowBandDelaySmootherL.setCurrentAndTargetValue(0.0f);
    lowBandDelaySmootherR.setCurrentAndTargetValue(0.0f);
    // 20 ms glide whenever the alignment target moves (crossover frequency or
    // diffuse-field EQ changes), so the bass anchor's delay never steps.
    lowBandAlignSmoother.reset(newSampleRate, 0.020);
    lowBandAlignSmoother.setCurrentAndTargetValue(0.0f);
    currentItdSeconds = 0.0f;

    // Set to impossible values to force first IR load
    lastIRUpdateAz = 999.0f;
    lastIRUpdateEl = 999.0f;
    lastIRUpdateQualityMode = -1;
    lastIRUpdateHrirLength = 0;
    lastIRUpdateInterpolation = -1;
    azimuthFilterInitialized = false;
    hrirUpdateCooldownSamples = 0;
    distanceFilterInitialized = false;
    erSuppressionSamples = 0;
    erSuppressionSamplesMax = 0;
    previousPosition = {currentAzimuth.load(), currentElevation.load()};
    targetPosition = previousPosition;
    effectiveRenderPosition = previousPosition;
    positionInterpolation = 1.0f;

    // Initialize dataset-dependent caches and initial HRIR
    onHrtfDatabaseChanged();

    // onHrtfDatabaseChanged() has now measured the alignment table; start the
    // crossover alignment at its value rather than ramping up to it from zero.
    lowBandAlignSmoother.setCurrentAndTargetValue(juce::jlimit(
        0.0f, static_cast<float>(kMaxLowBandAlignSamples), lookupCrossoverAlignmentSamples(crossoverFrequency.load())));
}

void BinauralProcessor::reset()
{
    leftEarConvA->reset();
    rightEarConvA->reset();
    sideLeftEarConvA->reset();
    sideRightEarConvA->reset();

    // Reset crossover filters
    lowMidLowPass.reset();
    lowMidHighPass.reset();

    // Reset multi-band air absorption
    airAbsorptionLowL.reset();
    airAbsorptionLowR.reset();
    airAbsorptionHighL.reset();
    airAbsorptionHighR.reset();

    // Reset early reflection buffers
    for (auto& buffer : erDelayBuffers)
        buffer.clear();
    erWritePositions.fill(0);

    monoBuffer.clear();
    lowFreqBuffer.clear();
    highFreqBuffer.clear();
    tempBuffer.clear();
    wetBuffer.clear();
    convBuffer.clear();
    sideBuffer.clear();
    sideConvBuffer.clear();
    erInputBuffer.clear();
    erAccumBuffer.clear();
    std::fill(lowBandDelayBuffer.begin(), lowBandDelayBuffer.end(), 0.0f);
    lowBandDelayWritePos = 0;
    lowBandDelaySmootherL.setCurrentAndTargetValue(0.0f);
    lowBandDelaySmootherR.setCurrentAndTargetValue(0.0f);
    // Restore the HRTF path's own group-delay alignment at the crossover.
    lowBandAlignSmoother.setCurrentAndTargetValue(juce::jlimit(
        0.0f, static_cast<float>(kMaxLowBandAlignSamples), lookupCrossoverAlignmentSamples(crossoverFrequency.load())));
    currentItdSeconds = 0.0f;

    azimuthSmoother.setCurrentAndTargetValue(currentAzimuth.load());
    elevationSmoother.setCurrentAndTargetValue(currentElevation.load());
    distanceSmoother.setCurrentAndTargetValue(currentDistance.load());
    // Initialize wet mix based on enabled state - if disabled, start at 0
    float initialWetMix = isEnabled.load() ? wetMix.load() : 0.0f;
    wetMixSmoother.setCurrentAndTargetValue(initialWetMix);
    sourceWidthSmoother.setCurrentAndTargetValue(sourceWidth.load());
    azimuthFiltered = azimuthSmoother.getCurrentValue();
    azimuthFilterInitialized = true;
    hrirUpdateCooldownSamples = 0;
    distanceFiltered = distanceSmoother.getCurrentValue();
    distanceFilterInitialized = true;
    erSuppressionSamples = 0;
    erSuppressionSamplesMax = 0;
    previousPosition = {currentAzimuth.load(), currentElevation.load()};
    targetPosition = previousPosition;
    effectiveRenderPosition = previousPosition;
    positionInterpolation = 1.0f;

    // Reset DVF filter states to prevent clicks
    dvfStateL[0] = 0.0f;
    dvfStateL[1] = 0.0f;
    dvfStateR[0] = 0.0f;
    dvfStateR[1] = 0.0f;
    dvfCoeffsL[0] = 1.0f;
    dvfCoeffsL[1] = 0.0f;
    dvfCoeffsL[2] = 0.0f;
    dvfCoeffsR[0] = 1.0f;
    dvfCoeffsR[1] = 0.0f;
    dvfCoeffsR[2] = 0.0f;
    dvfCoeffsTargetL[0] = 1.0f;
    dvfCoeffsTargetL[1] = 0.0f;
    dvfCoeffsTargetL[2] = 0.0f;
    dvfCoeffsTargetR[0] = 1.0f;
    dvfCoeffsTargetR[1] = 0.0f;
    dvfCoeffsTargetR[2] = 0.0f;
    nearFieldCompensationGain = 1.0f;
    nearFieldCompensationTarget = 1.0f;
    nearFieldSafetyEnv = 0.0f;
    nearFieldSafetyGain = 1.0f;
}

void BinauralProcessor::clearNaNLatchableState()
{
    // Only the recursive (IIR) state: crossover, air absorption and near-field
    // DVF histories. Delay lines and FIR convolution histories are
    // deliberately skipped - they flush themselves once upstream is clean.
    lowMidLowPass.reset();
    lowMidHighPass.reset();
    airAbsorptionLowL.reset();
    airAbsorptionLowR.reset();
    airAbsorptionHighL.reset();
    airAbsorptionHighR.reset();
    dvfStateL[0] = 0.0f;
    dvfStateL[1] = 0.0f;
    dvfStateR[0] = 0.0f;
    dvfStateR[1] = 0.0f;
}

int BinauralProcessor::nextPow2(int value)
{
    if (value <= 0)
        return 1;
    int v = 1;
    while (v < value)
        v <<= 1;
    return v;
}

void BinauralProcessor::updateHrirFftConfig(int newLength)
{
    hrirLength = juce::jlimit(1, HRIR_MAX_LENGTH, newLength);
    const int desiredFftSize = juce::jmax(HRIR_FFT_MIN_SIZE, nextPow2(hrirLength * 2));

    if (desiredFftSize != hrirFftSize)
    {
        hrirFftSize = desiredFftSize;
        hrirFftBins = hrirFftSize / 2 + 1;
        if (hrirFFT)
            hrirFFT->init(static_cast<size_t>(hrirFftSize));
    }
}

void BinauralProcessor::onHrtfDatabaseChanged()
{
    if (!hrtfDatabase || !hrtfDatabase->isLoaded())
        return;

    hrirLengthRaw = juce::jlimit(1, HRIR_MAX_LENGTH, hrtfDatabase->getHrirLength());
    updateHrirFftConfig(hrirLengthRaw);
    applyQualityHrirCap(getEffectiveQualityMode(hrtfQuality.load()));

    // The convolver is sized for the largest HRIR length this dataset can select at runtime.
    leftEarConvA->prepare(sampleRate, blockSize, hrirLengthRaw);
    rightEarConvA->prepare(sampleRate, blockSize, hrirLengthRaw);
    sideLeftEarConvA->prepare(sampleRate, blockSize, hrirLengthRaw);
    sideRightEarConvA->prepare(sampleRate, blockSize, hrirLengthRaw);

    lastIRUpdateAz = 999.0f;
    lastIRUpdateEl = 999.0f;
    lastIRUpdateWidth = -1.0f;
    lastIRUpdateQualityMode = -1;
    lastIRUpdateHrirLength = 0;
    lastIRUpdateInterpolation = -1;
    diffuseFieldComputed = false;
    itdsPrecomputed = false;

    computeDiffuseFieldResponse();
    precomputeGridITDs();
    computeCrossoverAlignmentTable();
    updateHRIRFromPosition();
}

bool BinauralProcessor::evaluateAdaptivePerformanceMode() noexcept
{
    bool active = adaptivePerformanceModeActive.load(std::memory_order_relaxed);
    const bool shouldEnter = sampleRate >= kAdaptiveEnterSampleRate && blockSize <= kAdaptiveEnterBlockSize;
    const bool shouldExit = sampleRate < kAdaptiveExitSampleRate || blockSize > kAdaptiveExitBlockSize;

    if (!active && shouldEnter)
        active = true;
    else if (active && shouldExit)
        active = false;

    adaptivePerformanceModeActive.store(active, std::memory_order_relaxed);
    return active;
}

int BinauralProcessor::getEffectiveQualityMode(int requestedQualityMode) noexcept
{
    const int clamped = juce::jlimit(0, 2, requestedQualityMode);
    return evaluateAdaptivePerformanceMode() ? 0 : clamped;
}

void BinauralProcessor::applyQualityHrirCap(int qualityMode)
{
    // Quality mode (2+) uses the full HRIR_MAX_LENGTH.
    int qualityCap = HRIR_MAX_LENGTH;
    if (qualityMode <= 0)
        qualityCap = HRIR_PERF_CAP;
    else if (qualityMode == 1)
        qualityCap = HRIR_BALANCED_CAP;

    hrirLength = juce::jlimit(1, qualityCap, hrirLengthRaw);

    if (hrtfDatabase)
        hrtfDatabase->setEffectiveHrirLength(hrirLength);
}

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
int BinauralProcessor::getCurrentHrirLengthForTest() const noexcept
{
    return hrirLength;
}

int BinauralProcessor::getRawHrirLengthForTest() const noexcept
{
    return hrirLengthRaw;
}

int BinauralProcessor::getConvolutionPreparedIrCapacityForTest() const noexcept
{
    return leftEarConvA ? leftEarConvA->getPreparedIrCapacityForTest() : 0;
}

int BinauralProcessor::getConvolutionActiveIrLengthForTest() const noexcept
{
    return leftEarConvA ? leftEarConvA->getActiveIrLengthForTest() : 0;
}

int BinauralProcessor::getConvolutionFftSizeForTest() const noexcept
{
    return leftEarConvA ? leftEarConvA->getFftSizeForTest() : 0;
}

float BinauralProcessor::getTargetHrirTailRmsForTest(int startSample) const noexcept
{
    const int start = juce::jlimit(0, hrirLength, startSample);
    double energy = 0.0;
    int count = 0;
    for (int i = start; i < hrirLength; ++i)
    {
        const float l = targetHRIR.leftEar[static_cast<size_t>(i)];
        const float r = targetHRIR.rightEar[static_cast<size_t>(i)];
        energy += static_cast<double>(l) * l + static_cast<double>(r) * r;
        count += 2;
    }
    return (count > 0) ? static_cast<float>(std::sqrt(energy / static_cast<double>(count))) : 0.0f;
}

BinauralProcessor::SofaConventionReport BinauralProcessor::getSofaConventionReportForTest() const
{
    return hrtfDatabase ? hrtfDatabase->getSofaConventionReport() : SofaConventionReport{};
}
#endif
