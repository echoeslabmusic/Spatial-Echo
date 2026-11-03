// BinauralProcessorNearField: Near-field distance effects (proximity boost, early reflections).
// Realtime-safety: no allocation/locks in processEarlyReflections()

#include "BinauralProcessor.h"

namespace
{
inline float readDelayLagrange4(const float* buffer, int bufferSize, float readPos)
{
    if (readPos < 0.0f)
        readPos += static_cast<float>(bufferSize);
    else if (readPos >= static_cast<float>(bufferSize))
        readPos -= static_cast<float>(bufferSize);

    int idx0 = static_cast<int>(std::floor(readPos));
    const float frac = readPos - static_cast<float>(idx0);
    // A position just below zero rounds to exactly bufferSize when wrapped
    // in float, which read the allocation padding past the last sample.
    if (idx0 >= bufferSize)
        idx0 -= bufferSize;

    int idxM1 = idx0 - 1;
    if (idxM1 < 0)
        idxM1 += bufferSize;
    int idx1 = idx0 + 1;
    if (idx1 >= bufferSize)
        idx1 -= bufferSize;
    int idx2 = idx0 + 2;
    if (idx2 >= bufferSize)
        idx2 -= bufferSize;

    const float xm1 = buffer[idxM1];
    const float x0 = buffer[idx0];
    const float x1 = buffer[idx1];
    const float x2 = buffer[idx2];

    const float c0 = x0;
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);

    return ((c3 * frac + c2) * frac + c1) * frac + c0;
}
}  // namespace

//==============================================================================
// Early Reflection Implementation
// Simulates first reflections from room surfaces
//==============================================================================

void BinauralProcessor::initEarlyReflections()
{
    // Define tap characteristics (delay, left gain, right gain in dB)
    // These simulate reflections from floor, ceiling, walls
    const float tapData[NUM_ER_TAPS][3] = {
        {12.0f, -6.0f, -8.0f},    // Floor reflection
        {18.0f, -8.0f, -6.0f},    // Ceiling reflection
        {25.0f, -10.0f, -4.0f},   // Left wall
        {28.0f, -4.0f, -10.0f},   // Right wall
        {38.0f, -12.0f, -12.0f},  // Back wall
        {52.0f, -14.0f, -14.0f}   // Diffuse tail
    };

    // Convert to linear gains and store
    for (size_t i = 0; i < erTaps.size(); ++i)
    {
        erTaps[i].delayMs = tapData[i][0];
        erTaps[i].gainL = std::pow(10.0f, tapData[i][1] / 20.0f);
        erTaps[i].gainR = std::pow(10.0f, tapData[i][2] / 20.0f);
    }

    // Calculate max delay in samples and allocate buffers
    float maxDelayMs = 60.0f;  // Maximum delay for early reflections
    maxErDelaySamples = static_cast<int>(maxDelayMs * sampleRate / 1000.0f) + 1;

    for (size_t i = 0; i < erDelayBuffers.size(); ++i)
    {
        erDelayBuffers[i].setSize(2, maxErDelaySamples);
        erDelayBuffers[i].clear();
        erWritePositions[i] = 0;
    }
}

void BinauralProcessor::processEarlyReflections(juce::AudioBuffer<float>& buffer, float distance)
{
    // Early reflections are more prominent at closer distances
    // At far distances (>5m), direct sound dominates
    // Moderate early reflections for room ambience
    float erMixTarget = juce::jmap(distance, 0.5f, 5.0f, 0.18f, 0.04f);
    erMixTarget = juce::jlimit(0.0f, 0.22f, erMixTarget);

    if (erSuppressionSamplesMax > 0 && erSuppressionSamples > 0)
    {
        float suppress = static_cast<float>(erSuppressionSamples) / static_cast<float>(erSuppressionSamplesMax);
        erMixTarget *= (1.0f - suppress);
    }

    const int numSamples = buffer.getNumSamples();
    if (numSamples > erInputBuffer.getNumSamples() || numSamples > erAccumBuffer.getNumSamples())
    {
        jassertfalse;
        return;
    }

    auto* leftOut = buffer.getWritePointer(0);
    auto* rightOut = buffer.getWritePointer(1);
    erInputBuffer.copyFrom(0, 0, buffer, 0, 0, numSamples);
    erInputBuffer.copyFrom(1, 0, buffer, 1, 0, numSamples);
    erAccumBuffer.clear(0, numSamples);
    auto* erAccumL = erAccumBuffer.getWritePointer(0);
    auto* erAccumR = erAccumBuffer.getWritePointer(1);
    const auto* erInputL = erInputBuffer.getReadPointer(0);
    const auto* erInputR = erInputBuffer.getReadPointer(1);

    // Calculate delay in samples (scales with distance - closer = more room)
    float delayScale = juce::jmap(distance, 0.5f, 3.0f, 1.2f, 0.6f);
    delayScale = juce::jlimit(0.5f, 1.5f, delayScale);

    std::array<float, NUM_ER_TAPS> targetDelaySamples{};
    std::array<float, NUM_ER_TAPS> targetGainL{};
    std::array<float, NUM_ER_TAPS> targetGainR{};

    const float samplesPerMs = static_cast<float>(sampleRate * 0.001);
    for (size_t tap = 0; tap < erTaps.size(); ++tap)
    {
        float delaySamples = erTaps[tap].delayMs * samplesPerMs * delayScale;
        delaySamples = juce::jlimit(0.0f, static_cast<float>(maxErDelaySamples - 1), delaySamples);
        targetDelaySamples[tap] = delaySamples;
        targetGainL[tap] = erTaps[tap].gainL * erMixTarget;
        targetGainR[tap] = erTaps[tap].gainR * erMixTarget;
    }

    if (!erSmoothingInitialized)
    {
        for (size_t tap = 0; tap < erTaps.size(); ++tap)
        {
            erDelaySamplesCurrent[tap] = targetDelaySamples[tap];
            erGainLCurrent[tap] = targetGainL[tap];
            erGainRCurrent[tap] = targetGainR[tap];
        }
        erSmoothingInitialized = true;
    }

    // Process each early reflection tap with fractional delay + per-sample smoothing
    for (size_t tap = 0; tap < erTaps.size(); ++tap)
    {
        auto& delayBuffer = erDelayBuffers[tap];
        int& writePos = erWritePositions[tap];

        auto* delayL = delayBuffer.getWritePointer(0);
        auto* delayR = delayBuffer.getWritePointer(1);

        float delaySamples = erDelaySamplesCurrent[tap];
        float gainL = erGainLCurrent[tap];
        float gainR = erGainRCurrent[tap];

        const float delayTarget = targetDelaySamples[tap];
        const float gainLTarget = targetGainL[tap];
        const float gainRTarget = targetGainR[tap];

        for (int i = 0; i < numSamples; ++i)
        {
            delaySamples += (delayTarget - delaySamples) * erDelaySmoothAlpha;
            gainL += (gainLTarget - gainL) * erGainSmoothAlpha;
            gainR += (gainRTarget - gainR) * erGainSmoothAlpha;

            // Feed each tap from the same pre-ER snapshot so taps do not recurse.
            delayL[writePos] = erInputL[i];
            delayR[writePos] = erInputR[i];

            // Read delayed sample with fractional interpolation
            float readPos = static_cast<float>(writePos) - delaySamples;
            float delayedL = readDelayLagrange4(delayL, maxErDelaySamples, readPos);
            float delayedR = readDelayLagrange4(delayR, maxErDelaySamples, readPos);

            // Add delayed signal with smoothed gains
            erAccumL[i] += delayedL * gainL;
            erAccumR[i] += delayedR * gainR;

            // Advance write position
            writePos++;
            if (writePos >= maxErDelaySamples)
                writePos = 0;
        }

        erDelaySamplesCurrent[tap] = delaySamples;
        erGainLCurrent[tap] = gainL;
        erGainRCurrent[tap] = gainR;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        leftOut[i] += erAccumL[i];
        rightOut[i] += erAccumR[i];
    }
}

//==============================================================================
// Near-Field Distance Variation Function (DVF) - Spagnol et al. (2017)
// Based on: Spagnol, Tavazzi, Avanzini (2017) "Distance rendering and perception
// of nearby virtual sound sources with a near-field filter model"
// Coefficients come from precomputed lookup tables; no allocation on the audio thread.
//==============================================================================

// DVF lookup tables (Spagnol et al. 2017)
// These are coefficients for high-shelf filter parameter interpolation
static constexpr float DVF_p11[19] = {12.97f, 13.19f, 12.13f, 11.19f, 9.91f,  8.328f, 6.493f, 4.455f, 2.274f, 0.018f,
                                      -2.24f, -4.43f, -6.49f, -8.34f, -9.93f, -11.3f, -12.2f, -12.8f, -13.0f};
static constexpr float DVF_p21[19] = {-9.69f, 234.2f, -11.2f, -9.03f, -7.87f, -7.42f, -7.31f, -7.28f, -7.29f, -7.48f,
                                      -8.04f, -9.23f, -11.6f, -17.4f, -48.4f, 9.149f, 1.905f, -0.75f, -1.32f};
static constexpr float DVF_q11[19] = {-1.14f, 18.48f, -1.25f, -1.02f, -0.83f, -0.67f, -0.5f,  -0.32f, -0.11f, -0.13f,
                                      0.395f, 0.699f, 1.084f, 1.757f, 4.764f, -0.64f, 0.109f, 0.386f, 0.45f};
static constexpr float DVF_q21[19] = {0.219f, -8.5f,  0.346f, 0.336f, 0.379f, 0.421f, 0.423f, 0.382f, 0.314f, 0.24f,
                                      0.177f, 0.132f, 0.113f, 0.142f, 0.462f, -0.14f, -0.08f, -0.06f, -0.05f};
static constexpr float DVF_p12[19] = {-4.39f, -4.31f, -4.18f, -4.01f, -3.87f, -4.1f,  -3.87f, -5.02f, -6.72f, -8.69f,
                                      -11.2f, -12.1f, -11.1f, -11.1f, -9.72f, -8.42f, -7.44f, -6.78f, -6.58f};
static constexpr float DVF_p22[19] = {2.123f, -2.78f, 4.224f, 3.039f, -0.57f, -34.7f, 3.271f, 0.023f, -8.96f, -58.4f,
                                      11.47f, 8.716f, 21.8f,  1.91f,  -0.04f, -0.66f, 0.395f, 2.662f, 3.387f};
static constexpr float DVF_q12[19] = {-0.55f, 0.59f,  -1.01f, -0.56f, 0.665f, 11.39f, -1.57f, -0.87f, 0.37f, 5.446f,
                                      -1.13f, -0.63f, -2.01f, 0.15f,  0.243f, 0.147f, -0.18f, -0.67f, -0.84f};
static constexpr float DVF_q22[19] = {-0.06f, -0.17f, -0.02f, -0.32f, -1.13f, -8.3f,  0.637f, 0.325f, -0.08f, -1.19f,
                                      0.103f, -0.12f, 0.098f, -0.4f,  -0.41f, -0.34f, -0.18f, 0.05f,  0.131f};
static constexpr float DVF_p13[19] = {0.457f, 0.455f, -0.87f, 0.465f, 0.494f, 0.549f, 0.663f, 0.691f, 3.507f, -27.4f,
                                      6.371f, 7.032f, 7.092f, 7.463f, 7.453f, 8.101f, 8.702f, 8.925f, 9.317f};
static constexpr float DVF_p23[19] = {-0.67f, 0.142f, 3404.f, -0.91f, -0.67f, -1.21f, -1.76f, 4.655f, 55.09f, 10336.f,
                                      1.735f, 40.88f, 23.86f, 102.8f, -6.14f, -18.1f, -9.05f, -9.03f, -6.89f};
static constexpr float DVF_p33[19] = {0.174f, -0.11f, -1699.f, 0.437f, 0.658f, 2.02f,  6.815f, 0.614f, 589.3f, 16818.f,
                                      -9.39f, -44.1f, -23.6f,  -92.3f, -1.81f, 10.54f, 0.532f, 0.285f, -2.08f};
static constexpr float DVF_q13[19] = {-1.75f, -0.01f, 7354.f, -2.18f, -1.2f,  -1.59f, -1.23f, -0.89f, 29.23f, 1945.f,
                                      -0.06f, 5.635f, 3.308f, 13.88f, -0.88f, -2.23f, -0.96f, -0.9f,  -0.57f};
static constexpr float DVF_q23[19] = {0.699f, -0.35f, -5350.f, 1.188f, 0.256f, 0.816f, 1.166f, 0.76f,  59.51f, 1707.f,
                                      -1.12f, -6.18f, -3.39f,  -12.7f, -0.19f, 1.295f, -0.02f, -0.08f, -0.4f};

static constexpr float DVF_HEAD_RADIUS = 0.09096f;                                    // meters
static constexpr float DVF_HEAD_DIM = 3.14159265f * (0.0875f / 0.09096f);             // pi * (a_0 / a_head)
static constexpr float DVF_SOS_DIV_2PI_A = 343.0f / (2.0f * 3.14159265f * 0.09096f);  // c / (2pi * a_head)

void BinauralProcessor::calcDVFCoeffsRT(float alpha, float rho, float* b0, float* b1, float* a1)
{
    // Clamp inputs
    alpha = juce::jlimit(0.0f, 180.0f, alpha);
    rho = juce::jmax(1.0f, rho);

    // Get table indices (10 degree steps)
    float thetaDiv10 = alpha / 10.0f;
    int idx1 = static_cast<int>(thetaDiv10);
    int idx2 = idx1 + 1;
    if (idx2 >= 19)
    {
        idx2 = 18;
        idx1 = 17;
    }
    float ifac = thetaDiv10 - static_cast<float>(idx1);

    float rhoSq = rho * rho;

    // Calculate shelf params at both indices (Eq 8, 13, 14 from paper)
    auto calcShelfParams = [&](int i, float& g0, float& gInf, float& fc)
    {
        g0 = (DVF_p11[i] * rho + DVF_p21[i]) / (rhoSq + DVF_q11[i] * rho + DVF_q21[i]);
        gInf = (DVF_p12[i] * rho + DVF_p22[i]) / (rhoSq + DVF_q12[i] * rho + DVF_q22[i]);
        float fc_tmp = (DVF_p13[i] * rhoSq + DVF_p23[i] * rho + DVF_p33[i]) / (rhoSq + DVF_q13[i] * rho + DVF_q23[i]);
        fc = fc_tmp * DVF_SOS_DIV_2PI_A;
    };

    float g0_1, gInf_1, fc_1, g0_2, gInf_2, fc_2;
    calcShelfParams(idx1, g0_1, gInf_1, fc_1);
    calcShelfParams(idx2, g0_2, gInf_2, fc_2);

    // Linear interpolation
    float g0 = g0_1 + (g0_2 - g0_1) * ifac;
    float gInf = gInf_1 + (gInf_2 - gInf_1) * ifac;
    float fc = fc_1 + (fc_2 - fc_1) * ifac;

    // Clamp to avoid unstable or ultra-high cutoffs at very close distances
    const float maxFc = 0.45f * static_cast<float>(sampleRate);
    fc = juce::jlimit(20.0f, maxFc, fc);
    g0 = juce::jlimit(-24.0f, 24.0f, g0);
    gInf = juce::jlimit(-24.0f, 24.0f, gInf);

    // Convert dB to magnitude
    float v0 = std::pow(10.0f, gInf / 20.0f);
    float g0_mag = std::pow(10.0f, g0 / 20.0f);
    float tanF = std::tan((DVF_HEAD_DIM / static_cast<float>(sampleRate)) * fc);
    float v0tanF = v0 * tanF;
    float a_c = (v0tanF - 1.0f) / (v0tanF + 1.0f);

    float v = (v0 - 1.0f) * 0.5f;
    float va_c = v * a_c;
    *b0 = g0_mag * (v - va_c + 1.0f);
    *b1 = g0_mag * (va_c - v + a_c);
    *a1 = a_c;
}

float BinauralProcessor::processDVFSample(float input, float* coeffs, float* state)
{
    // First-order IIR: y[n] = b0*x[n] + b1*x[n-1] - a1*y[n-1]
    float output = coeffs[0] * input + coeffs[1] * state[0] - coeffs[2] * state[1];
    state[0] = input;   // x[n-1]
    state[1] = output;  // y[n-1]
    return output;
}

void BinauralProcessor::updateNearFieldDVF(float distanceMeters)
{
    // Reference distance is 1m - no near-field effect at or beyond this
    const float referenceDistance = 1.0f;

    // Clamp distance to valid range (minimum 0.1m to avoid extreme effects)
    distanceMeters = juce::jlimit(0.1f, 10.0f, distanceMeters);

    if (distanceMeters >= referenceDistance - 1.0e-4f)
    {
        // At or beyond reference distance: bypass near-field filtering
        // Unity gain passthrough (no allocations!)
        dvfCoeffsTargetL[0] = 1.0f;
        dvfCoeffsTargetL[1] = 0.0f;
        dvfCoeffsTargetL[2] = 0.0f;
        dvfCoeffsTargetR[0] = 1.0f;
        dvfCoeffsTargetR[1] = 0.0f;
        dvfCoeffsTargetR[2] = 0.0f;
        nearFieldCompensationTarget = 1.0f;
        return;
    }

    // Normalize distance to head radius (rho = distance / head_radius)
    float rho = distanceMeters / DVF_HEAD_RADIUS;
    rho = juce::jmax(1.0f, rho);  // Minimum rho = 1 (at head surface)

    // Use distance-aware smoothed azimuth to reduce near-field modulation artifacts
    const float nearFieldAzimuth = azimuthFiltered;

    auto angularDistance = [](float a, float b)
    {
        float delta = a - b;
        while (delta > 180.0f)
            delta -= 360.0f;
        while (delta < -180.0f)
            delta += 360.0f;
        return std::abs(delta);
    };

    // Ear incidence angles: front is 90/90, hard-left is 0/180, hard-right is 180/0.
    const float alphaL = angularDistance(nearFieldAzimuth, -90.0f);
    const float alphaR = angularDistance(nearFieldAzimuth, 90.0f);

    calcDVFCoeffsRT(alphaL, rho, &dvfCoeffsTargetL[0], &dvfCoeffsTargetL[1], &dvfCoeffsTargetL[2]);
    calcDVFCoeffsRT(alphaR, rho, &dvfCoeffsTargetR[0], &dvfCoeffsTargetR[1], &dvfCoeffsTargetR[2]);

    auto endpointGain = [](const float* coeffs)
    {
        const float dcDenom = juce::jmax(1.0e-4f, std::abs(1.0f + coeffs[2]));
        const float nyquistDenom = juce::jmax(1.0e-4f, std::abs(1.0f - coeffs[2]));
        const float dcGain = std::abs((coeffs[0] + coeffs[1]) / dcDenom);
        const float nyquistGain = std::abs((coeffs[0] - coeffs[1]) / nyquistDenom);
        return juce::jmax(1.0f, dcGain, nyquistGain);
    };

    // The HRTF/DVF path already supplies close-source lateralization, so the
    // modeled boost is trimmed back: near-field depth does not spend headroom
    // on extra level.
    const float strongestModeledGain = juce::jmax(endpointGain(dvfCoeffsTargetL), endpointGain(dvfCoeffsTargetR));
    nearFieldCompensationTarget = juce::jlimit(0.1f, 1.0f, 1.0f / strongestModeledGain);
}
