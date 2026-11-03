// BinauralProcessor: 3D spatial audio using HRTFs; core binaural rendering engine.
// Realtime-safety: no allocation/locks in processBlock()

#include "BinauralProcessor.h"
#include "OouraFFT.h"
#include <cmath>

//==============================================================================
// BinauralProcessor Implementation
//==============================================================================

BinauralProcessor::BinauralProcessor()
{
    leftEarConvA = std::make_unique<ConvolutionEngine>();
    rightEarConvA = std::make_unique<ConvolutionEngine>();
    sideLeftEarConvA = std::make_unique<ConvolutionEngine>();
    sideRightEarConvA = std::make_unique<ConvolutionEngine>();

    // Initialize HRTF database (dataset load deferred to useDefaultHRTF)
    hrtfDatabase = std::make_unique<HRTFDatabase>();

    buildSpreadDecorrelators();

    // FFT for the diffuse-field analysis (size set by dataset length)
    hrirFFT = std::make_unique<OouraFFT>();
    updateHrirFftConfig(hrirLength);

    // Ramp lengths in steps until prepare() sets time-based ramps from the
    // sample rate.
    azimuthSmoother.reset(2);
    elevationSmoother.reset(2);
    distanceSmoother.reset(5);
    wetMixSmoother.reset(50);
    sourceWidthSmoother.reset(8);
}

BinauralProcessor::~BinauralProcessor() = default;

void BinauralProcessor::setEnabled(bool enabled)
{
    // Simply store the state - crossfade is handled by wetMixSmoother in processBlock
    isEnabled.store(enabled);
}

void BinauralProcessor::setAzimuth(float azimuthDegrees)
{
    // Clamp to valid range
    azimuthDegrees = juce::jlimit(-180.0f, 180.0f, azimuthDegrees);

    const float currentAz = currentAzimuth.load();
    const float targetEl = currentElevation.load();

    // Only trigger SLERP if position changed significantly
    if (angularDistanceDegrees(azimuthDegrees, currentAz) > 1.0f)
    {
        previousPosition = getCurrentSlerpPosition();
        targetPosition = {azimuthDegrees, targetEl};
        positionInterpolation = 0.0f;
    }

    currentAzimuth.store(azimuthDegrees);
}

void BinauralProcessor::setElevation(float elevationDegrees)
{
    // Clamp to valid range
    elevationDegrees = juce::jlimit(-90.0f, 90.0f, elevationDegrees);

    const float targetAz = currentAzimuth.load();
    const float currentEl = currentElevation.load();

    if (std::abs(elevationDegrees - currentEl) > 1.0f)
    {
        previousPosition = getCurrentSlerpPosition();
        targetPosition = {targetAz, elevationDegrees};
        positionInterpolation = 0.0f;
    }

    currentElevation.store(elevationDegrees);
}

void BinauralProcessor::setDistance(float distanceMeters)
{
    // Clamp to valid range
    distanceMeters = juce::jlimit(0.1f, 10.0f, distanceMeters);
    currentDistance.store(distanceMeters);
}

void BinauralProcessor::snapToPosition(float azimuthDegrees, float elevationDegrees, float distanceMeters)
{
    // Clamp to valid ranges
    azimuthDegrees = juce::jlimit(-180.0f, 180.0f, azimuthDegrees);
    elevationDegrees = juce::jlimit(-90.0f, 90.0f, elevationDegrees);
    distanceMeters = juce::jlimit(0.1f, 10.0f, distanceMeters);

    // Set position atomics directly (processBlock will read these)
    currentAzimuth.store(azimuthDegrees);
    currentElevation.store(elevationDegrees);
    currentDistance.store(distanceMeters);

    // Prime smoothers to prevent ramping
    azimuthSmoother.setCurrentAndTargetValue(azimuthDegrees);
    elevationSmoother.setCurrentAndTargetValue(elevationDegrees);
    distanceSmoother.setCurrentAndTargetValue(distanceMeters);
    sourceWidthSmoother.setCurrentAndTargetValue(sourceWidth.load());

    // Ensure spatial interpolation state indicates we're AT the target
    targetPosition = {azimuthDegrees, elevationDegrees};
    previousPosition = targetPosition;
    effectiveRenderPosition = targetPosition;
    positionInterpolation = 1.0f;

    // Also reset the azimuth filter to prevent transient artifacts
    azimuthFiltered = azimuthDegrees;
    azimuthFilterInitialized = true;
    distanceFiltered = distanceMeters;
    distanceFilterInitialized = true;
}

void BinauralProcessor::setWetMix(float wetPercent)
{
    // Convert percentage to 0-1 range
    wetPercent = juce::jlimit(0.0f, 100.0f, wetPercent) / 100.0f;
    wetMix.store(wetPercent);
}

void BinauralProcessor::setSourceWidth(float widthPercent)
{
    widthPercent = juce::jlimit(0.0f, 100.0f, widthPercent);
    sourceWidth.store(widthPercent);
}

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
void BinauralProcessor::primeSpatialStateForTest()
{
    const float azimuth = currentAzimuth.load();
    const float elevation = currentElevation.load();
    const float distance = currentDistance.load();
    const float currentWet = isEnabled.load() ? wetMix.load() : 0.0f;

    azimuthSmoother.setCurrentAndTargetValue(azimuth);
    elevationSmoother.setCurrentAndTargetValue(elevation);
    distanceSmoother.setCurrentAndTargetValue(distance);
    wetMixSmoother.setCurrentAndTargetValue(currentWet);
    sourceWidthSmoother.setCurrentAndTargetValue(sourceWidth.load());

    previousPosition = {azimuth, elevation};
    targetPosition = previousPosition;
    effectiveRenderPosition = previousPosition;
    positionInterpolation = 1.0f;
    azimuthFiltered = azimuth;
    azimuthFilterInitialized = true;
    distanceFiltered = distance;
    distanceFilterInitialized = true;
    hrirUpdateCooldownSamples = 0;
    erSuppressionSamples = 0;
    erSuppressionSamplesMax = 0;

    onHrtfDatabaseChanged();
}
#endif

void BinauralProcessor::setCrossoverFrequency(float freqHz)
{
    // Update the low/mid crossover frequency dynamically
    // Bass below this frequency bypasses HRTF processing for stability
    freqHz = juce::jlimit(20.0f, 500.0f, freqHz);
    crossoverFrequency.store(freqHz);
    lowMidLowPass.setCutoffFrequency(freqHz);
    lowMidHighPass.setCutoffFrequency(freqHz);
}

//==============================================================================
// SLERP (Spherical Linear Interpolation) Implementation
// Prevents front/back jumps when automating through +/-180 deg boundary
//==============================================================================

BinauralProcessor::SphericalCoord BinauralProcessor::slerpPosition(const SphericalCoord& from, const SphericalCoord& to,
                                                                   float t)
{
    if (t <= 0.0f)
        return from;
    if (t >= 1.0f)
        return to;

    // Work in a tangent basis instead of dividing by sin(acos(dot)). Near
    // opposite directions the float dot rounds to -1, and that formula can
    // jump straight to the side or reverse direction. Double precision keeps
    // the plane defined by nearby, non-antipodal endpoints well conditioned.
    const double radians = juce::MathConstants<double>::pi / 180.0;
    const double az1 = static_cast<double>(from.azimuth) * radians;
    const double el1 = static_cast<double>(from.elevation) * radians;
    const double az2 = static_cast<double>(to.azimuth) * radians;
    const double el2 = static_cast<double>(to.elevation) * radians;
    const double x1 = std::cos(el1) * std::sin(az1);
    const double y1 = std::sin(el1);
    const double z1 = std::cos(el1) * std::cos(az1);
    const double x2 = std::cos(el2) * std::sin(az2);
    const double y2 = std::sin(el2);
    const double z2 = std::cos(el2) * std::cos(az2);
    const double dot = juce::jlimit(-1.0, 1.0, x1 * x2 + y1 * y2 + z1 * z2);

    double x, y, z;
    if (dot > 0.9995)
    {
        x = x1 + static_cast<double>(t) * (x2 - x1);
        y = y1 + static_cast<double>(t) * (y2 - y1);
        z = z1 + static_cast<double>(t) * (z2 - z1);
    }
    else
    {
        double tx = x2 - dot * x1;
        double ty = y2 - dot * y1;
        double tz = z2 - dot * z1;
        const double tangentLength = std::sqrt(tx * tx + ty * ty + tz * tz);
        double angle;
        if (tangentLength > 1.0e-8)
        {
            tx /= tangentLength;
            ty /= tangentLength;
            tz /= tangentLength;
            angle = std::atan2(tangentLength, dot);
        }
        else
        {
            // Exact antipodes have no unique shortest arc. Follow the sign of
            // the requested azimuth change; pole-to-pole moves with unchanged
            // azimuth use that meridian. Both choices are unit tangents.
            const double azimuthDelta = std::remainder(static_cast<double>(to.azimuth) - from.azimuth, 360.0);
            if (std::abs(azimuthDelta) > 1.0e-6)
            {
                const double direction = azimuthDelta > 0.0 ? 1.0 : -1.0;
                tx = direction * std::cos(az1);
                ty = 0.0;
                tz = -direction * std::sin(az1);
            }
            else
            {
                const double direction = to.elevation > from.elevation ? 1.0 : -1.0;
                tx = -direction * std::sin(el1) * std::sin(az1);
                ty = direction * std::cos(el1);
                tz = -direction * std::sin(el1) * std::cos(az1);
            }
            angle = juce::MathConstants<double>::pi;
        }
        const double c = std::cos(static_cast<double>(t) * angle);
        const double s = std::sin(static_cast<double>(t) * angle);
        x = c * x1 + s * tx;
        y = c * y1 + s * ty;
        z = c * z1 + s * tz;
    }

    const double length = std::sqrt(x * x + y * y + z * z);
    return {static_cast<float>(std::atan2(x, z) / radians),
            static_cast<float>(std::asin(juce::jlimit(-1.0, 1.0, y / length)) / radians)};
}

float BinauralProcessor::angularDistanceDegrees(float a, float b)
{
    float delta = a - b;
    while (delta > 180.0f)
        delta -= 360.0f;
    while (delta < -180.0f)
        delta += 360.0f;
    return std::abs(delta);
}

BinauralProcessor::SphericalCoord BinauralProcessor::getCurrentSlerpPosition() const
{
    if (positionInterpolation < 1.0f)
        return slerpPosition(previousPosition, targetPosition, positionInterpolation);

    return {currentAzimuth.load(), currentElevation.load()};
}

void BinauralProcessor::advancePositionInterpolation(int numSamples)
{
    if (positionInterpolation >= 1.0f)
        return;

    const double rampSamples = std::max(1.0, sampleRate * kPositionInterpolationTimeSeconds);
    positionInterpolation =
        juce::jlimit(0.0f, 1.0f, positionInterpolation + static_cast<float>(numSamples / rampSamples));
    if (positionInterpolation >= 1.0f)
        previousPosition = targetPosition;
}

float BinauralProcessor::calculateDistanceAwareAzimuthAlpha(float distanceMeters, int numSamples) const
{
    const float distanceNorm = juce::jlimit(0.0f, 1.0f, (distanceMeters - 1.0f) / 2.25f);
    const float referenceAlpha = 0.2f + (1.0f - 0.2f) * distanceNorm;
    if (referenceAlpha >= 0.999f)
        return 1.0f;

    const double sr = std::max(1.0, sampleRate);
    const double samples = static_cast<double>(std::max(1, numSamples));
    const double referenceBlockSamples = 512.0;
    const double tauSeconds = -referenceBlockSamples / (sr * std::log(1.0 - static_cast<double>(referenceAlpha)));
    const double alpha = 1.0 - std::exp(-samples / (tauSeconds * sr));
    return juce::jlimit(0.0f, 1.0f, static_cast<float>(alpha));
}
