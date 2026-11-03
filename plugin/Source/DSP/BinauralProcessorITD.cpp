// BinauralProcessorITD: ITD estimation and low-band ITD delay application.
// Realtime-safety: no allocation/locks in applyLowBandITD()

#include "BinauralProcessor.h"
#include <algorithm>
#include <vector>

namespace
{
inline float readDelayLinear(const std::vector<float>& buffer, int writePos, float delaySamples)
{
    const int size = static_cast<int>(buffer.size());
    if (size <= 1)
        return 0.0f;

    float readPos = static_cast<float>(writePos) - delaySamples;
    while (readPos < 0.0f)
        readPos += static_cast<float>(size);
    while (readPos >= static_cast<float>(size))
        readPos -= static_cast<float>(size);

    int idx0 = static_cast<int>(std::floor(readPos));
    int idx1 = idx0 + 1;
    if (idx1 >= size)
        idx1 -= size;
    float frac = readPos - static_cast<float>(idx0);
    const float s0 = buffer[static_cast<size_t>(idx0)];
    const float s1 = buffer[static_cast<size_t>(idx1)];
    return s0 + (s1 - s0) * frac;
}
}  // namespace

//==============================================================================
// ITD Estimation with 750Hz LPF and Cross-Correlation
// Low-frequency cross-correlation method - more accurate than peak detection
//==============================================================================
float BinauralProcessor::estimateITDSamples(const HRIRData& hrir) const
{
    // =========================================================================
    // Step 1: Apply 750Hz lowpass filter (2nd-order IIR biquad)
    // ITD cues are primarily in low frequencies, LPF removes high-freq noise
    // Filter design: fc=750Hz, Q=0.7071 (Butterworth)
    // =========================================================================
    const float fc = 750.0f;
    const float Q = 0.7071f;
    const float fs = static_cast<float>(sampleRate);
    const float K = std::tan(juce::MathConstants<float>::pi * fc / fs);
    const float KK = K * K;
    const float D = KK * Q + K + Q;

    // Biquad coefficients
    const float b0 = (KK * Q) / D;
    const float b1 = (2.0f * KK * Q) / D;
    const float b2 = (KK * Q) / D;
    const float a1 = (2.0f * Q * (KK - 1.0f)) / D;
    const float a2 = (KK * Q - K + Q) / D;

    // Filter states for left and right
    float Wz1L = 0.0f, Wz2L = 0.0f;
    float Wz1R = 0.0f, Wz2R = 0.0f;

    // Filtered HRIRs
    std::array<float, HRIR_MAX_LENGTH> filteredL{};
    std::array<float, HRIR_MAX_LENGTH> filteredR{};

    for (size_t n = 0; n < static_cast<size_t>(hrirLength); ++n)
    {
        // Left ear - Direct Form II biquad
        float wnL = hrir.leftEar[n] - a1 * Wz1L - a2 * Wz2L;
        filteredL[n] = b0 * wnL + b1 * Wz1L + b2 * Wz2L;
        Wz2L = Wz1L;
        Wz1L = wnL;

        // Right ear
        float wnR = hrir.rightEar[n] - a1 * Wz1R - a2 * Wz2R;
        filteredR[n] = b0 * wnR + b1 * Wz1R + b2 * Wz2R;
        Wz2R = Wz1R;
        Wz1R = wnR;
    }

    // =========================================================================
    // Step 2: Cross-correlation between filtered L and R
    // xcorr_len = 2*hrirLength - 1
    // =========================================================================
    const int xcorrLen = 2 * hrirLength - 1;
    std::array<float, HRIR_MAX_LENGTH * 2> xcorr{};

    for (int lag = -(hrirLength - 1); lag < hrirLength; ++lag)
    {
        float sum = 0.0f;
        for (int n = 0; n < hrirLength; ++n)
        {
            int m = n + lag;
            if (m >= 0 && m < hrirLength)
                sum += filteredL[static_cast<size_t>(n)] * filteredR[static_cast<size_t>(m)];
        }
        xcorr[static_cast<size_t>(lag + hrirLength - 1)] = sum;
    }

    // =========================================================================
    // Step 3: Find peak of cross-correlation
    // =========================================================================
    int maxIdx = hrirLength - 1;  // Center (zero lag)
    float maxVal = xcorr[static_cast<size_t>(maxIdx)];

    for (int i = 0; i < xcorrLen; ++i)
    {
        if (xcorr[static_cast<size_t>(i)] > maxVal)
        {
            maxVal = xcorr[static_cast<size_t>(i)];
            maxIdx = i;
        }
    }

    // Convert index to ITD in samples: positive = right leads (left delayed)
    float itdSamples = static_cast<float>(hrirLength - 1 - maxIdx);

    // =========================================================================
    // Step 4: Apply physical bounds (±0.707ms, ~31 samples at 44.1kHz)
    // Physical bounds for human head geometry
    // =========================================================================
    const float maxITDSamples = MAX_ITD_SECONDS * fs;
    itdSamples = juce::jlimit(-maxITDSamples, maxITDSamples, itdSamples);

    return itdSamples;
}

//==============================================================================
// Pre-compute ITDs for all grid points (called once at load time)
//==============================================================================
void BinauralProcessor::precomputeGridITDs()
{
    if (!hrtfDatabase || !hrtfDatabase->isLoaded())
        return;

    const auto& gridPoints = hrtfDatabase->getGridPoints();
    precomputedITDs.resize(gridPoints.size());

    for (size_t i = 0; i < gridPoints.size(); ++i)
    {
        const auto& hrir = hrtfDatabase->getHRIRByGridIndex(static_cast<int>(i));
        float itdSamples = estimateITDSamples(hrir);
        // Store as seconds for interpolation compatibility
        float itdSeconds = itdSamples / static_cast<float>(sampleRate);
        float delayL = 0.0f;
        float delayR = 0.0f;
        if (hrtfDatabase->getSofaDelays(static_cast<int>(i), delayL, delayR))
            itdSeconds += (delayR - delayL);
        precomputedITDs[i] = itdSeconds;
    }

    if (gridPoints.size() > 3)
    {
        // Spatially smooth ITD field to suppress estimator outliers on sparse grids.
        std::vector<float> smoothed(precomputedITDs.size(), 0.0f);
        const float degToRad = juce::MathConstants<float>::pi / 180.0f;
        const float sigma = 25.0f * degToRad;
        const float maxDist = 75.0f * degToRad;
        const float inv2Sigma2 = 1.0f / (2.0f * sigma * sigma);

        for (size_t i = 0; i < gridPoints.size(); ++i)
        {
            const auto& p = gridPoints[i];
            float sum = 0.0f;
            float weightSum = 0.0f;
            for (size_t j = 0; j < gridPoints.size(); ++j)
            {
                const auto& q = gridPoints[j];
                float dot = p.x * q.x + p.y * q.y + p.z * q.z;
                dot = juce::jlimit(-1.0f, 1.0f, dot);
                const float dist = std::acos(dot);
                if (dist > maxDist)
                    continue;
                const float w = std::exp(-(dist * dist) * inv2Sigma2);
                sum += w * precomputedITDs[j];
                weightSum += w;
            }
            smoothed[i] = (weightSum > 1.0e-9f) ? (sum / weightSum) : precomputedITDs[i];
        }

        precomputedITDs.swap(smoothed);
    }

    itdsPrecomputed = true;
}

float BinauralProcessor::interpolateITD(float azimuth, float elevation) const
{
    if (!itdsPrecomputed || !hrtfDatabase || !hrtfDatabase->isLoaded())
        return 0.0f;

    const auto& gridPoints = hrtfDatabase->getGridPoints();
    if (gridPoints.empty())
        return 0.0f;

    const float degToRad = juce::MathConstants<float>::pi / 180.0f;
    const float azRad = azimuth * degToRad;
    const float elRad = elevation * degToRad;
    const float tx = std::cos(elRad) * std::sin(azRad);
    const float ty = std::cos(elRad) * std::cos(azRad);
    const float tz = std::sin(elRad);

    // Gaussian weighting on angular distance to ensure smooth ITD variation.
    const float sigma = 25.0f * degToRad;
    const float inv2Sigma2 = 1.0f / (2.0f * sigma * sigma);
    float weightSum = 0.0f;
    float itdSum = 0.0f;

    for (size_t i = 0; i < gridPoints.size(); ++i)
    {
        const auto& p = gridPoints[i];
        float dot = tx * p.x + ty * p.y + tz * p.z;
        dot = juce::jlimit(-1.0f, 1.0f, dot);
        const float dist = std::acos(dot);
        const float w = std::exp(-(dist * dist) * inv2Sigma2);
        weightSum += w;
        itdSum += w * precomputedITDs[i];
    }

    return (weightSum > 1.0e-9f) ? (itdSum / weightSum) : 0.0f;
}

void BinauralProcessor::updateLowBandITDTargets(float itdSeconds)
{
    const float maxItD = MAX_ITD_SECONDS;
    itdSeconds = juce::jlimit(-maxItD, maxItD, itdSeconds);
    const float itdSamples = itdSeconds * static_cast<float>(sampleRate);

    float delayL = 0.0f;
    float delayR = 0.0f;
    if (itdSamples > 0.0f)
        delayL = itdSamples;  // Right leads -> delay left
    else if (itdSamples < 0.0f)
        delayR = -itdSamples;  // Left leads -> delay right

    lowBandDelaySmootherL.setTargetValue(delayL);
    lowBandDelaySmootherR.setTargetValue(delayR);
}

const std::array<float, BinauralProcessor::kAlignTableSize> BinauralProcessor::kAlignTableFreqHz = {
    20.0f, 50.0f, 100.0f, 150.0f, 220.0f, 320.0f, 420.0f, 500.0f};

float BinauralProcessor::lookupCrossoverAlignmentSamples(float freqHz) const
{
    if (!alignTableValid)
        return 0.0f;

    const bool includeDfe = diffuseFieldEQEnabled.load() && !isBypassedForTest(TestBypass::DiffuseEqAlignment);
    auto valueAt = [&](int i)
    {
        return hrirAlignDelayTable[static_cast<size_t>(i)] +
               (includeDfe ? dfeAlignDelayTable[static_cast<size_t>(i)] : 0.0f);
    };

    if (freqHz <= kAlignTableFreqHz[0])
        return valueAt(0);
    if (freqHz >= kAlignTableFreqHz[kAlignTableSize - 1])
        return valueAt(kAlignTableSize - 1);

    for (int i = 1; i < kAlignTableSize; ++i)
    {
        const float hi = kAlignTableFreqHz[static_cast<size_t>(i)];
        if (freqHz <= hi)
        {
            const float lo = kAlignTableFreqHz[static_cast<size_t>(i - 1)];
            const float t = (hi > lo) ? ((freqHz - lo) / (hi - lo)) : 0.0f;
            return valueAt(i - 1) + (valueAt(i) - valueAt(i - 1)) * t;
        }
    }
    return valueAt(kAlignTableSize - 1);
}

void BinauralProcessor::updateLowBandAlignment()
{
    // Align the bass anchor with the HRIR's group delay at the crossover.
    // The hybrid convolver adds no callback-dependent alignment delay.
    const float hrtfDelay = lookupCrossoverAlignmentSamples(crossoverFrequency.load());
    const float target = juce::jlimit(0.0f, static_cast<float>(kMaxLowBandAlignSamples), hrtfDelay);
    lowBandAlignSmoother.setTargetValue(target);
}

void BinauralProcessor::applyLowBandITD(const float* input, float* outL, float* outR, int numSamples)
{
    if (lowBandDelayBuffer.empty())
    {
        for (int i = 0; i < numSamples; ++i)
        {
            outL[i] = input[i];
            outR[i] = input[i];
        }
        return;
    }

    const int size = lowBandDelayBufferSize;
    int writePos = lowBandDelayWritePos;
    const float maxDelay = static_cast<float>(size - 2);

    for (int i = 0; i < numSamples; ++i)
    {
        lowBandDelayBuffer[static_cast<size_t>(writePos)] = input[i];

        const float align = lowBandAlignSmoother.getNextValue();
        const float delayL = juce::jlimit(0.0f, maxDelay, lowBandDelaySmootherL.getNextValue() + align);
        const float delayR = juce::jlimit(0.0f, maxDelay, lowBandDelaySmootherR.getNextValue() + align);

        outL[i] = readDelayLinear(lowBandDelayBuffer, writePos, delayL);
        outR[i] = readDelayLinear(lowBandDelayBuffer, writePos, delayR);

        ++writePos;
        if (writePos >= size)
            writePos = 0;
    }

    lowBandDelayWritePos = writePos;
}

//==============================================================================
// Crossover alignment table.
//
// Measures the group delay, in samples, of the HRTF path at each frequency in
// kAlignTableFreqHz, so the low band can be delayed to match and the crossover
// sums flat. Group delay is read off the phase slope of the DTFT, which is the
// quantity that governs how the two bands add; the HRIR's ONSET is a different
// and much smaller number (~22 vs ~93 samples at 150 Hz on the bundled set),
// because a measured HRIR carries large excess phase at low frequency.
//
// Per direction the LEADING ear is used, so the interaural difference is not
// counted twice: the lagging ear's extra delay is the ITD, which the low band
// applies separately. The median over directions is taken because a handful of
// directions sit on a spectral null where the phase-slope estimate is noisy.
//
// Load-time only (called from onHrtfDatabaseChanged), so allocation is fine.
//==============================================================================
void BinauralProcessor::computeCrossoverAlignmentTable()
{
    alignTableValid = false;
    hrirAlignDelayTable.fill(0.0f);
    dfeAlignDelayTable.fill(0.0f);

    if (!hrtfDatabase || !hrtfDatabase->isLoaded() || sampleRate <= 0.0)
        return;

    const auto& gridPoints = hrtfDatabase->getGridPoints();
    if (gridPoints.empty())
        return;

    const int length = juce::jlimit(1, HRIR_MAX_LENGTH, hrirLengthRaw);

    // Phase slope across +-kDelta Hz. The span is wide enough that the estimate
    // is not dominated by numerical noise and narrow enough that the phase step
    // stays inside +-pi for any delay below 1/(4*kDelta) = 12.5 ms.
    constexpr double kDelta = 20.0;

    auto dtftPhase = [&](const float* ir, int n, double freq)
    {
        double re = 0.0, im = 0.0;
        for (int k = 0; k < n; ++k)
        {
            const double a = -2.0 * juce::MathConstants<double>::pi * freq * k / sampleRate;
            re += ir[k] * std::cos(a);
            im += ir[k] * std::sin(a);
        }
        return std::atan2(im, re);
    };

    auto groupDelay = [&](const float* ir, int n, double freq)
    {
        double d = dtftPhase(ir, n, freq + kDelta) - dtftPhase(ir, n, freq - kDelta);
        while (d > juce::MathConstants<double>::pi)
            d -= 2.0 * juce::MathConstants<double>::pi;
        while (d < -juce::MathConstants<double>::pi)
            d += 2.0 * juce::MathConstants<double>::pi;
        return -d / (2.0 * juce::MathConstants<double>::pi * 2.0 * kDelta) * sampleRate;
    };

    std::vector<double> perDirection;
    perDirection.reserve(gridPoints.size());

    for (int f = 0; f < kAlignTableSize; ++f)
    {
        const double freq = static_cast<double>(kAlignTableFreqHz[static_cast<size_t>(f)]);
        perDirection.clear();

        for (size_t idx = 0; idx < gridPoints.size(); ++idx)
        {
            const auto& hrir = hrtfDatabase->getHRIRByGridIndex(static_cast<int>(idx));

            double energyL = 0.0, energyR = 0.0;
            for (int i = 0; i < length; ++i)
            {
                energyL +=
                    static_cast<double>(hrir.leftEar[static_cast<size_t>(i)]) * hrir.leftEar[static_cast<size_t>(i)];
                energyR +=
                    static_cast<double>(hrir.rightEar[static_cast<size_t>(i)]) * hrir.rightEar[static_cast<size_t>(i)];
            }
            if (energyL <= 1.0e-16 && energyR <= 1.0e-16)
                continue;

            const float* leading = (energyL >= energyR) ? hrir.leftEar.data() : hrir.rightEar.data();
            const double gd = groupDelay(leading, length, freq);
            if (std::isfinite(gd))
                perDirection.push_back(gd);
        }

        if (perDirection.empty())
            return;

        std::nth_element(perDirection.begin(), perDirection.begin() + static_cast<long>(perDirection.size() / 2),
                         perDirection.end());
        const double median = perDirection[perDirection.size() / 2];
        hrirAlignDelayTable[static_cast<size_t>(f)] =
            juce::jlimit(0.0f, static_cast<float>(kMaxLowBandAlignSamples), static_cast<float>(median));

        // The diffuse-field correction filter is in series with the HRIR when
        // it is enabled, and group delays of cascaded filters add.
        if (diffuseFieldComputed && diffuseFieldFirLength > 0)
        {
            const double gdL = groupDelay(diffuseFieldResponseL.data(), diffuseFieldFirLength, freq);
            const double gdR = groupDelay(diffuseFieldResponseR.data(), diffuseFieldFirLength, freq);
            const double gdDfe = 0.5 * (gdL + gdR);
            if (std::isfinite(gdDfe))
                dfeAlignDelayTable[static_cast<size_t>(f)] =
                    juce::jlimit(0.0f, static_cast<float>(kMaxLowBandAlignSamples), static_cast<float>(gdDfe));
        }
    }

    alignTableValid = true;
}

void BinauralProcessor::applyFractionalDelay(float* ir, int length, float delaySamples)
{
    if (delaySamples <= 0.0001f || ir == nullptr || length <= 0)
        return;

    length = juce::jlimit(1, HRIR_MAX_LENGTH, length);
    const float maxDelay = static_cast<float>(length - 2);
    delaySamples = juce::jlimit(0.0f, maxDelay, delaySamples);

    std::array<float, HRIR_MAX_LENGTH> input{};
    for (int i = 0; i < length; ++i)
        input[static_cast<size_t>(i)] = ir[i];

    for (int i = 0; i < length; ++i)
    {
        float readPos = static_cast<float>(i) - delaySamples;
        float sample = 0.0f;

        if (readPos >= 0.0f && readPos < static_cast<float>(length - 1))
        {
            int idx = static_cast<int>(readPos);
            float frac = readPos - static_cast<float>(idx);
            float s0 = input[static_cast<size_t>(idx)];
            float s1 = input[static_cast<size_t>(idx + 1)];
            sample = s0 + (s1 - s0) * frac;
        }
        else if (readPos >= 0.0f && readPos < static_cast<float>(length))
        {
            sample = input[static_cast<size_t>(readPos)];
        }

        ir[static_cast<size_t>(i)] = sample;
    }
}
