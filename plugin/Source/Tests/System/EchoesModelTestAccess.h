// EchoesModelTestAccess: Test-only access to EchoesModel internals (level probes, branch gains, stage timing, fault
//   injection, head-timing capture and the isolated tape record/playback chains).

#pragma once

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "../../DSP/EchoesModel.h"
#include <vector>

struct EchoesModelTestAccess
{
    // Echo branch level after the tone stack, before the output trim.
    static bool getEchoPathPreTrimMetrics(const EchoesModel& model, float& peak, float& rms) noexcept;
    static void enableEchoPathPreTrimProbe(EchoesModel& model, bool enabled) noexcept;
    static void resetEchoPathPreTrimProbe(EchoesModel& model) noexcept;

    static float feedbackCompensationGain(const EchoesModel& model) noexcept;
    static float noiseFeedbackIntensity(const EchoesModel& model) noexcept;
    static bool getLastBranchBalanceGains(const EchoesModel& model, float& echoGain, float& reverbGain) noexcept;

    // Reverb input and recovered-return levels, one slot per routing mode (0-2).
    static void enableReverbInputProbe(EchoesModel& model, bool enabled) noexcept;
    static void resetReverbInputProbe(EchoesModel& model) noexcept;
    static bool getReverbInputMetrics(const EchoesModel& model, int routingMode, float& peak, float& rms) noexcept;
    static void enableReverbReturnProbe(EchoesModel& model, bool enabled) noexcept;
    static void resetReverbReturnProbe(EchoesModel& model) noexcept;
    static bool getReverbReturnMetrics(const EchoesModel& model, int routingMode, float& peak, float& rms) noexcept;
    static bool copyReverbReturnBuffer(const EchoesModel& model, std::vector<std::vector<float>>& out, int numSamples);

    // Auto-panic fault injectors. setWetBusFault adds a constant to every
    // post-mix sample (a broken downstream stage emitting loud finite
    // garbage); 0 disables. poisonTapeLoop corrupts the tape buffer directly
    // (e.g. with NaN), the internal-corruption wedge the hard flush must cure.
    static void setWetBusFault(EchoesModel& model, float amplitude) noexcept;
    static void poisonTapeLoop(EchoesModel& model, float value);

    static void enableStageTimingCapture(EchoesModel& model, bool enabled) noexcept;
    static void resetStageTimingCapture(EchoesModel& model) noexcept;
    static bool getStageTimingSnapshot(const EchoesModel& model, EchoesModel::StageTimingSnapshot& out) noexcept;

    static void enableHeadTimingCapture(EchoesModel& model, bool enabled);
    static bool getHeadTimingSnapshot(const EchoesModel& model, TapeDelayProcessor::HeadTimingSnapshot& out);
    static void processRecordChainOnly(EchoesModel& model, std::vector<std::vector<float>>& buffer, int blockSize);
    static void processPlaybackOnly(EchoesModel& model, std::vector<std::vector<float>>& buffer, int blockSize);
};

#endif
