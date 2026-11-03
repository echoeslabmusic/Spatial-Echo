// FeedbackTapeCalibrationTests: Isolated tape-record small-signal measurement and calibration gate.

#include "FeedbackTapeCalibrationTests.h"

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "../../DSP/TapeFlavors.h"
#include "../Common/TestUtilities.h"
#include "../../DSP/TapeRecordSaturator.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace SpatialEchoTests::TapeEmulation
{
namespace
{
constexpr int kCaptureSamples = 4096;
constexpr float kDeterminismLimitDb = 0.05f;

struct OperatingPoint
{
    juce::String name;
    float drive = 0.5f;
    float saturation = 0.5f;
    float bias = 0.5f;
    int flavorIndex = -1;
    float driveKnob = -1.0f;
    bool driveHoldout = false;
    int driveTestIndex = -1;
    bool applyMeasuredMakeup = false;
};

struct ToneMeasurement
{
    float requestedHz = 0.0f;
    float actualHz = 0.0f;
    float inputDbfs = 0.0f;
    float gainDb = -999.0f;
    float thdDb = -999.0f;
    float residualDb = -999.0f;
    bool finite = false;
};

juce::File artifactDirectory()
{
    auto directory = SpatialEchoTests::artifactDirectory().getChildFile("tape-calibration");
    directory.createDirectory();
    return directory;
}

double rounded(double value, double scale = 10000.0)
{
    if (!std::isfinite(value))
        return -999.0;
    return std::round(value * scale) / scale;
}

float linearToDb(float value)
{
    return juce::Decibels::gainToDecibels(std::max(value, 1.0e-12f), -240.0f);
}

ToneMeasurement measureTone(const OperatingPoint& point, float sampleRate, float requestedHz, float inputDbfs)
{
    echoes::TapeRecordSaturator processor;
    processor.prepare(sampleRate, 1);
    processor.setCharacter(point.drive, point.saturation, point.bias);
    if (point.applyMeasuredMakeup && point.flavorIndex >= 0)
    {
        processor.setMakeupGainDb(TapeFlavors::getTapeRecordMakeupDb(point.flavorIndex, point.driveKnob));
    }

    const int cycles =
        std::max(4, static_cast<int>(std::round(requestedHz * static_cast<float>(kCaptureSamples) / sampleRate)));
    const float actualHz = static_cast<float>(cycles) * sampleRate / static_cast<float>(kCaptureSamples);
    const double phaseIncrement = juce::MathConstants<double>::twoPi * static_cast<double>(actualHz / sampleRate);
    const float inputAmplitude = juce::Decibels::decibelsToGain(inputDbfs);
    const int warmupSamples = static_cast<int>(std::ceil(0.060 * sampleRate));
    double phase = 0.0;
    bool finite = true;

    for (int sample = 0; sample < warmupSamples; ++sample)
    {
        const float input = inputAmplitude * static_cast<float>(std::sin(phase));
        const float output = processor.processMono(input, 0);
        finite = finite && std::isfinite(output);
        phase += phaseIncrement;
        if (phase >= juce::MathConstants<double>::twoPi)
            phase -= juce::MathConstants<double>::twoPi;
    }

    double sinProjection = 0.0;
    double cosProjection = 0.0;
    std::array<double, 4> harmonicSinProjections{};
    std::array<double, 4> harmonicCosProjections{};
    double sum = 0.0;
    double sumSquares = 0.0;
    for (int sample = 0; sample < kCaptureSamples; ++sample)
    {
        const double sine = std::sin(phase);
        const double cosine = std::cos(phase);
        const float input = inputAmplitude * static_cast<float>(sine);
        const float output = processor.processMono(input, 0);
        finite = finite && std::isfinite(output);
        sinProjection += static_cast<double>(output) * sine;
        cosProjection += static_cast<double>(output) * cosine;
        for (int harmonic = 2; harmonic <= 5; ++harmonic)
        {
            const double harmonicPhase = phase * static_cast<double>(harmonic);
            const auto index = static_cast<size_t>(harmonic - 2);
            harmonicSinProjections[index] += static_cast<double>(output) * std::sin(harmonicPhase);
            harmonicCosProjections[index] += static_cast<double>(output) * std::cos(harmonicPhase);
        }
        sum += output;
        sumSquares += static_cast<double>(output) * output;
        phase += phaseIncrement;
        if (phase >= juce::MathConstants<double>::twoPi)
            phase -= juce::MathConstants<double>::twoPi;
    }

    const double scale = 2.0 / static_cast<double>(kCaptureSamples);
    const double fundamentalAmplitude = scale * std::hypot(sinProjection, cosProjection);
    const double dc = sum / static_cast<double>(kCaptureSamples);
    const double meanSquare = sumSquares / static_cast<double>(kCaptureSamples);
    const double fundamentalPower = 0.5 * fundamentalAmplitude * fundamentalAmplitude;
    double harmonicPower = 0.0;
    for (size_t index = 0; index < harmonicSinProjections.size(); ++index)
    {
        const double harmonicAmplitude =
            scale * std::hypot(harmonicSinProjections[index], harmonicCosProjections[index]);
        harmonicPower += 0.5 * harmonicAmplitude * harmonicAmplitude;
    }
    const double residualPower = std::max(0.0, meanSquare - fundamentalPower - dc * dc);
    const double fundamentalRms = fundamentalAmplitude / std::sqrt(2.0);

    ToneMeasurement result;
    result.requestedHz = requestedHz;
    result.actualHz = actualHz;
    result.inputDbfs = inputDbfs;
    result.gainDb = linearToDb(static_cast<float>(fundamentalAmplitude) / std::max(inputAmplitude, 1.0e-12f));
    result.thdDb = linearToDb(static_cast<float>(std::sqrt(harmonicPower / std::max(fundamentalPower, 1.0e-24))));
    result.residualDb = linearToDb(static_cast<float>(std::sqrt(residualPower)) /
                                   std::max(static_cast<float>(fundamentalRms), 1.0e-12f));
    result.finite = finite && std::isfinite(result.gainDb) && std::isfinite(result.thdDb) &&
                    std::isfinite(result.residualDb) && fundamentalAmplitude > 1.0e-12;
    return result;
}

std::vector<OperatingPoint> measurementPoints()
{
    std::vector<OperatingPoint> points{
        {"grid-centre", 0.50f, 0.50f, 0.50f},     {"drive-low", 0.05f, 0.50f, 0.50f},
        {"drive-high", 0.95f, 0.50f, 0.50f},      {"saturation-low", 0.50f, 0.05f, 0.50f},
        {"saturation-high", 0.50f, 0.95f, 0.50f}, {"bias-low", 0.50f, 0.50f, 0.00f},
        {"bias-high", 0.50f, 0.50f, 1.00f},
    };

    constexpr std::array<float, 2> driveCorners{0.05f, 0.95f};
    constexpr std::array<float, 2> saturationCorners{0.05f, 0.95f};
    constexpr std::array<float, 2> biasCorners{0.0f, 1.0f};
    for (size_t driveIndex = 0; driveIndex < driveCorners.size(); ++driveIndex)
    {
        for (size_t saturationIndex = 0; saturationIndex < saturationCorners.size(); ++saturationIndex)
        {
            for (size_t biasIndex = 0; biasIndex < biasCorners.size(); ++biasIndex)
            {
                points.push_back({"corner-d" + juce::String(static_cast<int>(driveIndex)) + "-s" +
                                      juce::String(static_cast<int>(saturationIndex)) + "-b" +
                                      juce::String(static_cast<int>(biasIndex)),
                                  driveCorners[driveIndex], saturationCorners[saturationIndex],
                                  biasCorners[biasIndex]});
            }
        }
    }

    constexpr std::array<float, 5> flavorDriveKnobs{0.0f, 0.225f, 0.45f, 0.725f, 1.0f};
    for (int flavor = 0; flavor < static_cast<int>(TapeFlavor::NumFlavors); ++flavor)
    {
        for (const float driveKnob : flavorDriveKnobs)
        {
            const auto resolved = TapeFlavors::resolve(flavor, driveKnob);
            points.push_back(
                {"flavor-" + juce::String(TapeFlavors::get(flavor).name) + "-drive-" + juce::String(driveKnob, 3),
                 resolved.drive, resolved.saturation, resolved.bias, flavor, driveKnob, false, -1});
        }
    }
    for (auto& point : points)
    {
        point.drive = juce::jlimit(0.05f, 0.95f, point.drive);
        point.saturation = juce::jlimit(0.05f, 0.95f, point.saturation);
        point.bias = juce::jlimit(0.0f, 1.0f, point.bias);
    }
    return points;
}

float geometricMeanGainDb(const OperatingPoint& point, float sampleRate, float inputDbfs, bool& finite, float& spreadDb,
                          float& determinismDb)
{
    constexpr std::array<float, 3> frequencies{250.0f, 1000.0f, 4000.0f};
    std::array<float, 3> gains{};
    finite = true;
    determinismDb = 0.0f;
    for (size_t index = 0; index < frequencies.size(); ++index)
    {
        const auto first = measureTone(point, sampleRate, frequencies[index], inputDbfs);
        const auto second = measureTone(point, sampleRate, frequencies[index], inputDbfs);
        gains[index] = first.gainDb;
        finite = finite && first.finite && second.finite;
        determinismDb = std::max(determinismDb, std::abs(first.gainDb - second.gainDb));
    }
    const auto [minimum, maximum] = std::minmax_element(gains.begin(), gains.end());
    spreadDb = *maximum - *minimum;
    return (gains[0] + gains[1] + gains[2]) / 3.0f;
}
}  // namespace

juce::String runTapeRecordSmallSignalMatrixTest(SpatialEchoAudioProcessor&)
{
    constexpr std::array<float, 6> sampleRates{44100.0f, 48000.0f, 88200.0f, 96000.0f, 176400.0f, 192000.0f};
    constexpr std::array<float, 3> frequencies{250.0f, 1000.0f, 4000.0f};
    constexpr std::array<float, 3> levels{-60.0f, -48.0f, -36.0f};

    auto* root = new juce::DynamicObject();
    root->setProperty("test", "TapeRecordSmallSignalMatrix");
    root->setProperty("calibration_position", "standalone record saturator before optional measured makeup");
    root->setProperty("default_makeup_linear", 1.0);
    root->setProperty("default_makeup_db", 0.0);
    root->setProperty("capture_samples", kCaptureSamples);
    juce::Array<juce::var> rows;
    bool allFinite = true;
    float maximumRepeatDifferenceDb = 0.0f;
    float maximumFrequencySpreadDb = 0.0f;
    int measurementCount = 0;
    const auto points = measurementPoints();
    constexpr int kExpectedOperatingPointCount = 40;  // 15 generic + 5 flavors x 5 Drive knots.
    juce::StringArray uniquePointNames;
    for (const auto& point : points)
        uniquePointNames.addIfNotAlreadyThere(point.name);
    const bool operatingPointsComplete = static_cast<int>(points.size()) == kExpectedOperatingPointCount &&
                                         uniquePointNames.size() == kExpectedOperatingPointCount;
    const int expectedMeasurementCount =
        static_cast<int>(kExpectedOperatingPointCount * sampleRates.size() * levels.size() * frequencies.size());

    for (const auto& point : points)
    {
        for (const float sampleRate : sampleRates)
        {
            for (const float level : levels)
            {
                std::array<ToneMeasurement, frequencies.size()> firstMeasurements{};
                std::array<ToneMeasurement, frequencies.size()> secondMeasurements{};
                std::array<float, frequencies.size()> gains{};
                for (size_t index = 0; index < frequencies.size(); ++index)
                {
                    const float frequency = frequencies[index];
                    firstMeasurements[index] = measureTone(point, sampleRate, frequency, level);
                    secondMeasurements[index] = measureTone(point, sampleRate, frequency, level);
                    gains[index] = firstMeasurements[index].gainDb;
                }
                const auto [minimumGain, maximumGain] = std::minmax_element(gains.begin(), gains.end());
                const float frequencySpreadDb = *maximumGain - *minimumGain;
                maximumFrequencySpreadDb = std::max(maximumFrequencySpreadDb, frequencySpreadDb);

                for (size_t index = 0; index < frequencies.size(); ++index)
                {
                    const float frequency = frequencies[index];
                    const auto& first = firstMeasurements[index];
                    const auto& second = secondMeasurements[index];
                    const float repeatDifference = std::abs(first.gainDb - second.gainDb);
                    allFinite = allFinite && first.finite && second.finite;
                    maximumRepeatDifferenceDb = std::max(maximumRepeatDifferenceDb, repeatDifference);

                    auto* row = new juce::DynamicObject();
                    row->setProperty("point", point.name);
                    row->setProperty("sample_rate", rounded(sampleRate, 1.0));
                    row->setProperty("drive", rounded(point.drive));
                    row->setProperty("saturation", rounded(point.saturation));
                    row->setProperty("bias", rounded(point.bias));
                    row->setProperty("requested_hz", rounded(frequency, 100.0));
                    row->setProperty("actual_hz", rounded(first.actualHz, 100.0));
                    row->setProperty("input_dbfs", rounded(level, 100.0));
                    row->setProperty("measured_current_gain_db", rounded(first.gainDb));
                    row->setProperty("thd_db_rel", rounded(first.thdDb));
                    row->setProperty("residual_db_rel", rounded(first.residualDb));
                    row->setProperty("frequency_spread_db", rounded(frequencySpreadDb));
                    row->setProperty("repeat_difference_db", rounded(repeatDifference));
                    row->setProperty("required_additional_compensation_db", rounded(-first.gainDb));
                    row->setProperty("required_total_makeup_db", rounded(-first.gainDb));
                    row->setProperty("finite", first.finite && second.finite);
                    rows.add(juce::var(row));
                    ++measurementCount;
                }
            }
        }
    }

    const bool complete = operatingPointsComplete && measurementCount == expectedMeasurementCount &&
                          rows.size() == expectedMeasurementCount;
    root->setProperty("expected_operating_point_count", kExpectedOperatingPointCount);
    root->setProperty("operating_point_count", static_cast<int>(points.size()));
    root->setProperty("unique_operating_point_count", uniquePointNames.size());
    root->setProperty("expected_measurement_count", expectedMeasurementCount);
    root->setProperty("measurement_count", measurementCount);
    root->setProperty("maximum_repeat_difference_db", rounded(maximumRepeatDifferenceDb));
    root->setProperty("maximum_frequency_spread_db", rounded(maximumFrequencySpreadDb));
    root->setProperty("rows", rows);
    root->setProperty("finite", allFinite);
    root->setProperty("complete", complete);

    const juce::File directory = artifactDirectory();
    const juce::File artifact = directory.getChildFile("tape_record_small_signal.json");
    const bool directoryReady = directory.isDirectory();
    const bool wroteArtifact = directoryReady && artifact.replaceWithText(juce::JSON::toString(juce::var(root), false));
    const bool deterministic = maximumRepeatDifferenceDb <= kDeterminismLimitDb;
    const bool passed = complete && allFinite && deterministic && wroteArtifact;

    juce::String result;
    result << "TapeRecordSmallSignalMatrix: count=" << measurementCount << "/" << expectedMeasurementCount
           << " maxRepeatDiff=" << juce::String(maximumRepeatDifferenceDb, 4) << "dB"
           << " maxFrequencySpread=" << juce::String(maximumFrequencySpreadDb, 2) << "dB"
           << " json=" << artifact.getFullPathName() << (passed ? " [PASS]" : " [FAIL]");
    return result;
}

juce::String runTapeRecordMakeupCalibrationTest(SpatialEchoAudioProcessor&)
{
    struct RatePoint
    {
        float sampleRate;
        bool holdout;
    };
    struct DrivePoint
    {
        float driveKnob;
        bool holdout;
    };

    constexpr std::array<RatePoint, 6> sampleRates{{{44100.0f, false},
                                                    {48000.0f, false},
                                                    {88200.0f, true},
                                                    {96000.0f, false},
                                                    {176400.0f, true},
                                                    {192000.0f, false}}};
    constexpr std::array<float, 3> levels{-60.0f, -48.0f, -36.0f};
    constexpr std::array<DrivePoint, 9> drivePoints{{{0.0000f, false},
                                                     {0.1125f, true},
                                                     {0.2250f, false},
                                                     {0.3375f, true},
                                                     {0.4500f, false},
                                                     {0.5875f, true},
                                                     {0.7250f, false},
                                                     {0.8625f, true},
                                                     {1.0000f, false}}};
    std::vector<OperatingPoint> points;
    for (int flavor = 0; flavor < static_cast<int>(TapeFlavor::NumFlavors); ++flavor)
    {
        for (size_t driveIndex = 0; driveIndex < drivePoints.size(); ++driveIndex)
        {
            const auto drivePoint = drivePoints[driveIndex];
            const float driveKnob = drivePoint.driveKnob;
            const auto resolved = TapeFlavors::resolve(flavor, driveKnob);
            points.push_back({juce::String(TapeFlavors::get(flavor).name) + "-drive-" + juce::String(driveKnob, 4),
                              resolved.drive, resolved.saturation, resolved.bias, flavor, driveKnob, drivePoint.holdout,
                              static_cast<int>(driveIndex), true});
        }
    }

    constexpr int flavorCount = static_cast<int>(TapeFlavor::NumFlavors);
    std::array<std::array<float, drivePoints.size()>, flavorCount> referenceGains{};
    for (auto& gains : referenceGains)
        gains.fill(std::numeric_limits<float>::quiet_NaN());

    bool allFinite = true;
    bool gainWithinTarget = true;
    bool spreadWithinTarget = true;
    bool deterministic = true;
    float worstAbsoluteGainDb = 0.0f;
    float worstSpreadDb = 0.0f;
    float worstDeterminismDb = 0.0f;
    juce::String worstGainAt;
    int gainFailureCount = 0;
    int spreadFailureCount = 0;
    int determinismFailureCount = 0;
    int measurementCells = 0;

    for (const auto& point : points)
    {
        for (size_t rateIndex = 0; rateIndex < sampleRates.size(); ++rateIndex)
        {
            const auto ratePoint = sampleRates[rateIndex];
            for (size_t levelIndex = 0; levelIndex < levels.size(); ++levelIndex)
            {
                const float level = levels[levelIndex];
                bool finite = false;
                float spreadDb = std::numeric_limits<float>::infinity();
                float determinismDb = std::numeric_limits<float>::infinity();
                const float meanGainDb =
                    geometricMeanGainDb(point, ratePoint.sampleRate, level, finite, spreadDb, determinismDb);
                const float gainToleranceDb = (point.driveHoldout || ratePoint.holdout) ? 0.75f : 0.50f;
                const bool gainOk = finite && std::abs(meanGainDb) <= gainToleranceDb;
                const bool spreadOk = finite && spreadDb <= 1.0f;
                const bool determinismOk = finite && determinismDb <= kDeterminismLimitDb;
                allFinite = allFinite && finite;
                gainWithinTarget = gainWithinTarget && gainOk;
                spreadWithinTarget = spreadWithinTarget && spreadOk;
                deterministic = deterministic && determinismOk;
                if (!gainOk)
                    ++gainFailureCount;
                if (!spreadOk)
                    ++spreadFailureCount;
                if (!determinismOk)
                    ++determinismFailureCount;
                if (std::abs(meanGainDb) > worstAbsoluteGainDb)
                {
                    worstAbsoluteGainDb = std::abs(meanGainDb);
                    worstGainAt = point.name + "@" + juce::String(ratePoint.sampleRate / 1000.0f, 1) + "k/" +
                                  juce::String(level, 0) + "dB=" + juce::String(meanGainDb, 2) + "dB";
                }
                worstSpreadDb = std::max(worstSpreadDb, spreadDb);
                worstDeterminismDb = std::max(worstDeterminismDb, determinismDb);
                if (rateIndex == 1 && levelIndex == 1 && point.flavorIndex >= 0 && point.driveTestIndex >= 0)
                {
                    referenceGains[static_cast<size_t>(point.flavorIndex)][static_cast<size_t>(point.driveTestIndex)] =
                        meanGainDb;
                }
                ++measurementCells;
            }
        }
    }

    // Prove that the public lookup is continuous at and between its explicit
    // nonuniform knots. The nine DSP measurement positions above verify the
    // absolute residual; treating their widely spaced level differences as
    // discontinuities would conflate a smooth record-stage gain slope with a control
    // step.
    constexpr int continuitySteps = 1024;
    float maximumAdjacentGainStepDb = 0.0f;
    juce::String worstAdjacentAt;
    bool adjacentStepsFinite = true;
    for (int flavor = 0; flavor < flavorCount; ++flavor)
    {
        float previous = TapeFlavors::getTapeRecordMakeupDb(flavor, 0.0f);
        adjacentStepsFinite = adjacentStepsFinite && std::isfinite(previous);
        for (int step = 1; step <= continuitySteps; ++step)
        {
            const float drive = static_cast<float>(step) / static_cast<float>(continuitySteps);
            const float current = TapeFlavors::getTapeRecordMakeupDb(flavor, drive);
            const bool finite = std::isfinite(current);
            adjacentStepsFinite = adjacentStepsFinite && finite;
            if (!finite)
                continue;
            const float stepDb = std::abs(current - previous);
            if (stepDb > maximumAdjacentGainStepDb)
            {
                maximumAdjacentGainStepDb = stepDb;
                worstAdjacentAt = juce::String(TapeFlavors::get(flavor).name) + " " +
                                  juce::String(static_cast<float>(step - 1) / static_cast<float>(continuitySteps), 4) +
                                  "->" + juce::String(drive, 4);
            }
            previous = current;
        }
    }
    const bool adjacencyWithinTarget = adjacentStepsFinite && maximumAdjacentGainStepDb <= 0.25f;
    const int expectedCells = static_cast<int>(points.size() * sampleRates.size() * levels.size());
    const bool complete = measurementCells == expectedCells;
    const bool passed =
        allFinite && gainWithinTarget && spreadWithinTarget && deterministic && adjacencyWithinTarget && complete;
    juce::String result;
    result << "TapeRecordMakeupCalibration: cells=" << measurementCells << "/" << expectedCells
           << " worstGain=" << juce::String(worstAbsoluteGainDb, 2) << "dB"
           << " at=" << worstGainAt << " worstSpread=" << juce::String(worstSpreadDb, 2) << "dB"
           << " maxRepeatDiff=" << juce::String(worstDeterminismDb, 4) << "dB"
           << " maxAdjacentStep=" << juce::String(maximumAdjacentGainStepDb, 2) << "dB at=" << worstAdjacentAt
           << " failures(gain/spread/determinism)=" << gainFailureCount << "/" << spreadFailureCount << "/"
           << determinismFailureCount << (passed ? " [PASS]" : " [FAIL]");
    if (!passed)
    {
        result << " refs48k-48dB={";
        for (int flavor = 0; flavor < flavorCount; ++flavor)
        {
            if (flavor > 0)
                result << ";";
            result << TapeFlavors::get(flavor).name << ":";
            for (size_t driveIndex = 0; driveIndex < drivePoints.size(); ++driveIndex)
            {
                if (driveIndex > 0)
                    result << ",";
                result << juce::String(referenceGains[static_cast<size_t>(flavor)][driveIndex], 3);
            }
        }
        result << "}";
    }
    return result;
}
}  // namespace SpatialEchoTests::TapeEmulation

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
