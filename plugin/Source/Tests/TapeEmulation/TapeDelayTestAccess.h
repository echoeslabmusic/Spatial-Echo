// TapeDelayTestAccess: Test-only access to TapeDelayProcessor internals (mode crossfade, filter cutoffs, ping-pong
//   seed, smoothing coefficients, fault injection, head-timing capture and the isolated record/playback chains).

#pragma once

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "../../DSP/TapeDelay.h"
#include <vector>

struct TapeNoiseProcessorTestAccess
{
    static float feedbackIntensity(const TapeNoiseProcessor& noise) noexcept { return noise.Intensity; }
};

struct TapeDelayTestAccess
{
    static bool isModeCrossfadeActive(const TapeDelayProcessor& tape) noexcept { return tape.crossfadeActive; }
    static float modeCrossfadeProgress(const TapeDelayProcessor& tape) noexcept { return tape.crossfadeProgress; }
    static int currentStereoMode(const TapeDelayProcessor& tape) noexcept { return tape.StereoMode; }
    static int previousStereoMode(const TapeDelayProcessor& tape) noexcept { return tape.PreviousStereoMode; }
    static float playbackFilterCutoff(const TapeDelayProcessor& tape) noexcept { return tape.playbackFilterCutoff; }
    static float feedbackFilterCutoff(const TapeDelayProcessor& tape) noexcept { return tape.feedbackFilterCutoff; }
    static float recordFilterCutoff(const TapeDelayProcessor& tape) noexcept { return tape.recordFilterCutoff; }
    static float pingPongSeedAngle(const TapeDelayProcessor& tape) noexcept { return tape.pingPongSeedAngle; }
    static int pingPongSeedRampSamples(const TapeDelayProcessor& tape) noexcept { return tape.pingPongSeedRampSamples; }
    static int pingPongSeedRampRemaining(const TapeDelayProcessor& tape) noexcept
    {
        return tape.pingPongSeedRampRemaining;
    }
    static float pingPongSeedPhasorNorm(const TapeDelayProcessor& tape) noexcept
    {
        return std::sqrt(tape.pingPongSeedCos * tape.pingPongSeedCos + tape.pingPongSeedSin * tape.pingPongSeedSin);
    }

    static float intensitySmoothed(const TapeDelayProcessor& tape) noexcept { return tape.IntensitySmoothed; }
    static float intensitySmoothingCoefficient(const TapeDelayProcessor& tape) noexcept
    {
        return tape.intensitySmoothCoeff;
    }
    // Exposed so a test can pin these to a time constant rather than to a
    // per-sample number.
    static float feedbackAverageCoefficient(const TapeDelayProcessor& tape) noexcept { return tape.feedbackAvgCoeff; }
    static float tempoSyncGlideCoefficient(const TapeDelayProcessor& tape) noexcept { return tape.tempoSyncGlideCoeff; }
    static float noiseFeedbackIntensity(const TapeDelayProcessor& tape) noexcept
    {
        return TapeNoiseProcessorTestAccess::feedbackIntensity(tape.NoiseProcessor);
    }
    static float feedbackCompensationGain(const TapeDelayProcessor& tape) noexcept;

    // Fault injector for the auto-panic watchdog tests: writes `value` (e.g.
    // NaN) straight into each channel's tape buffer, simulating internal state
    // corruption that the record-path sanitizers can't cause.
    static void poisonTapeBuffer(TapeDelayProcessor& tape, float value);

    // Head timing capture: the next sample whose input peak exceeds 0.01 after
    // enabling records the live head timing into a snapshot.
    static void enableHeadTimingCapture(TapeDelayProcessor& tape, bool enabled);
    static bool getHeadTimingSnapshot(const TapeDelayProcessor& tape, TapeDelayProcessor::HeadTimingSnapshot& out);

    // Isolated chains for spectral tests: the record path alone (record
    // filters, record-point saturation, DC block) and the playback path alone
    // (a copy of ProcessBuffer's read side without the feedback write).
    static void processRecordChainOnly(TapeDelayProcessor& tape, std::vector<std::vector<float>>& buffer,
                                       int blockSize);
    static void processPlaybackOnly(TapeDelayProcessor& tape, std::vector<std::vector<float>>& buffer, int blockSize);
};

#endif
