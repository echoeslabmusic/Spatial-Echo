// SpatialRoutingTests: Spatial routing, placement control sources, motion and the spatial metric battery.

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "SystemTests.h"
#include "../Common/TestUtilities.h"
#include "../../PluginProcessor.h"
#include "../../DSP/EchoesModel.h"
#include "../../UI/Components/DivisionMenuGrid.h"
#include "../../UI/Components/SpatialMotionSection.h"
#include "../../UI/Components/SpatialPresetCatalog.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <tuple>
#include <vector>

namespace SpatialEchoTests::System
{

juce::String runSpatialControlSourceArbitrationTest(SpatialEchoAudioProcessor& processor)
{
    constexpr double fs = 48000.0;
    constexpr int smallBlockSize = 128;
    constexpr int largeBlockSize = 16384;

    auto runCase = [&](int blockSize)
    {
        processor.noteSpatialManualInteraction();
        processor.prepareToPlay(fs, blockSize);

        auto& vts = processor.getValueTreeState();
        auto* azimuthParam = dynamic_cast<juce::AudioParameterFloat*>(vts.getParameter("SpatialAzimuth"));
        if (azimuthParam == nullptr)
            return false;

        juce::AudioBuffer<float> buffer(2, blockSize);
        buffer.clear();
        juce::MidiBuffer midi;

        auto processOnce = [&](juce::MidiBuffer& midiIn)
        {
            buffer.clear();
            processor.processBlock(buffer, midiIn);
        };

        auto setAzimuth = [&](float valueDegrees)
        { azimuthParam->setValueNotifyingHost(azimuthParam->convertTo0to1(valueDegrees)); };

        // Prime internal source-tracking baseline.
        processOnce(midi);

        // A manual interaction before prepareToPlay should not leak into the next run.
        setAzimuth(-20.0f);
        processOnce(midi);
        const bool reprepareOk =
            processor.getSpatialControlSource() == SpatialEchoAudioProcessor::SpatialControlSource::Automation;

        // Manual UI path
        processor.noteSpatialManualInteraction();
        setAzimuth(35.0f);
        processOnce(midi);
        const bool manualOk =
            processor.getSpatialControlSource() == SpatialEchoAudioProcessor::SpatialControlSource::Manual;

        const int blocksToExhaustManualWindow = static_cast<int>(std::ceil((fs * 0.45) / blockSize));
        for (int i = 0; i < blocksToExhaustManualWindow; ++i)
            processOnce(midi);

        // Automation path (parameter move after the manual activity window has elapsed in processed samples)
        setAzimuth(-40.0f);
        processOnce(midi);
        const bool automationOk =
            processor.getSpatialControlSource() == SpatialEchoAudioProcessor::SpatialControlSource::Automation;

        // MIDI path
        juce::MidiBuffer midiCc;
        midiCc.addEvent(juce::MidiMessage::controllerEvent(1, 21, 96), 0);  // CC21 = azimuth
        processOnce(midiCc);
        const bool midiOk =
            processor.getSpatialControlSource() == SpatialEchoAudioProcessor::SpatialControlSource::MIDI;

        processor.flushPendingHostUpdatesForUi();
        return reprepareOk && manualOk && automationOk && midiOk;
    };

    const bool smallOk = runCase(smallBlockSize);
    const bool largeOk = runCase(largeBlockSize);
    const bool passed = smallOk && largeOk;

    juce::String result;
    result << "SpatialControlSource: block128=" << (smallOk ? "ok" : "fail")
           << " block16384=" << (largeOk ? "ok" : "fail") << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runSpatialMotionSourceTest(SpatialEchoAudioProcessor& processor)
{
    constexpr double fs = 48000.0;
    constexpr int blockSize = 128;

    processor.prepareToPlay(fs, blockSize);

    auto& vts = processor.getValueTreeState();
    auto* motionPattern = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter("SpatialPattern"));
    auto* motionDepth = dynamic_cast<juce::AudioParameterFloat*>(vts.getParameter("SpatialDepth"));
    auto* motionRate = dynamic_cast<juce::AudioParameterFloat*>(vts.getParameter("SpatialRate"));
    auto* motionRetrigger = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter("SpatialRestart"));
    if (motionPattern == nullptr || motionDepth == nullptr || motionRate == nullptr || motionRetrigger == nullptr)
    {
        return "SpatialMotionSource: required parameters missing [FAIL]";
    }

    motionPattern->setValueNotifyingHost(motionPattern->convertTo0to1(1.0f));  // Orbit pattern enables motion
    motionDepth->setValueNotifyingHost(motionDepth->convertTo0to1(60.0f));
    motionRate->setValueNotifyingHost(motionRate->convertTo0to1(1.0f));
    motionRetrigger->setValueNotifyingHost(motionRetrigger->convertTo0to1(1.0f));  // Transport

    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    buffer.clear();
    processor.processBlock(buffer, midi);  // baseline/init
    buffer.clear();
    processor.processBlock(buffer, midi);  // source should now reflect motion

    const bool sourceOk =
        processor.getSpatialControlSource() == SpatialEchoAudioProcessor::SpatialControlSource::Motion;
    const bool retriggerOk = (motionRetrigger->getIndex() == 1);
    const bool passed = sourceOk && retriggerOk;

    juce::String result;
    result << "SpatialMotionSource: source=" << (sourceOk ? "motion" : "other")
           << " retrigger=" << (retriggerOk ? "transport" : "other") << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runSpatialMotionContinuityStressTest(SpatialEchoAudioProcessor& processor)
{
    constexpr double fs = 48000.0;
    constexpr int blockSize = 256;
    constexpr float inputAmp = 0.2f;
    constexpr float inputHz = 440.0f;
    constexpr float durationSecPerMode = 1.0f;
    const int blocksPerMode = juce::jmax(1, static_cast<int>((fs * durationSecPerMode) / blockSize));

    processor.prepareToPlay(fs, blockSize);

    auto& vts = processor.getValueTreeState();
    auto* binauralEnable = dynamic_cast<juce::AudioParameterBool*>(vts.getParameter("SpatialEnabled"));
    auto* binauralAz = dynamic_cast<juce::AudioParameterFloat*>(vts.getParameter("SpatialAzimuth"));
    auto* binauralEl = dynamic_cast<juce::AudioParameterFloat*>(vts.getParameter("SpatialElevation"));
    auto* binauralDist = dynamic_cast<juce::AudioParameterFloat*>(vts.getParameter("SpatialDistance"));
    auto* motionPattern = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter("SpatialPattern"));
    auto* motionRate = dynamic_cast<juce::AudioParameterFloat*>(vts.getParameter("SpatialRate"));
    auto* motionSync = dynamic_cast<juce::AudioParameterBool*>(vts.getParameter("SpatialSync"));
    auto* motionDepth = dynamic_cast<juce::AudioParameterFloat*>(vts.getParameter("SpatialDepth"));
    auto* motionDivision = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter("SpatialDivision"));

    if (binauralEnable == nullptr || binauralAz == nullptr || binauralEl == nullptr || binauralDist == nullptr ||
        motionPattern == nullptr || motionRate == nullptr || motionSync == nullptr || motionDepth == nullptr ||
        motionDivision == nullptr)
    {
        return "SpatialMotionContinuity: required parameters missing [FAIL]";
    }

    auto setBool = [](juce::AudioParameterBool* p, bool v) { p->setValueNotifyingHost(v ? 1.0f : 0.0f); };
    auto setFloat = [](juce::AudioParameterFloat* p, float v) { p->setValueNotifyingHost(p->convertTo0to1(v)); };
    auto setChoice = [](juce::AudioParameterChoice* p, int index)
    {
        const int maxIndex = juce::jmax(0, p->choices.size() - 1);
        const int clamped = juce::jlimit(0, maxIndex, index);
        const float norm = (maxIndex > 0) ? (static_cast<float>(clamped) / static_cast<float>(maxIndex)) : 0.0f;
        p->setValueNotifyingHost(norm);
    };

    setBool(binauralEnable, true);
    setFloat(binauralAz, 0.0f);
    setFloat(binauralEl, 0.0f);
    setFloat(binauralDist, 1.0f);
    setChoice(motionPattern, 1);  // Orbit (non-Still enables motion)
    setBool(motionSync, false);

    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    const float phaseInc = 2.0f * juce::MathConstants<float>::pi * inputHz / static_cast<float>(fs);
    float oscPhase = 0.0f;

    struct MotionMetrics
    {
        float maxDelta = 0.0f;
        double sumDelta = 0.0;
        int deltaCount = 0;
        double sumSq = 0.0;
        int sampleCount = 0;
        float peak = 0.0f;
        int worstBlock = -1;
        int worstSample = -1;
        bool finite = true;
        bool signal = false;
    };

    auto processAndMeasure = [&](MotionMetrics& metrics, float& prevL, float& prevR, bool& hasPrev, int blockIndex)
    {
        auto* left = buffer.getWritePointer(0);
        auto* right = buffer.getWritePointer(1);
        for (int i = 0; i < blockSize; ++i)
        {
            const float sample = inputAmp * std::sin(oscPhase);
            oscPhase += phaseInc;
            if (oscPhase >= 2.0f * juce::MathConstants<float>::pi)
                oscPhase -= 2.0f * juce::MathConstants<float>::pi;
            left[i] = sample;
            right[i] = sample;
        }

        processor.processBlock(buffer, midi);

        const float* outL = buffer.getReadPointer(0);
        const float* outR = buffer.getReadPointer(1);
        for (int i = 0; i < blockSize; ++i)
        {
            const float l = outL[i];
            const float r = outR[i];

            if (!std::isfinite(l) || !std::isfinite(r))
                metrics.finite = false;

            const float absL = std::abs(l);
            const float absR = std::abs(r);
            metrics.peak = juce::jmax(metrics.peak, juce::jmax(absL, absR));
            metrics.signal = metrics.signal || (absL > 1.0e-5f) || (absR > 1.0e-5f);

            metrics.sumSq += static_cast<double>(l) * l + static_cast<double>(r) * r;
            metrics.sampleCount += 2;

            if (hasPrev)
            {
                const float dL = std::abs(l - prevL);
                const float dR = std::abs(r - prevR);
                const float d = juce::jmax(dL, dR);
                if (d > metrics.maxDelta)
                {
                    metrics.maxDelta = d;
                    metrics.worstBlock = blockIndex;
                    metrics.worstSample = i;
                }
                metrics.sumDelta += dL + dR;
                metrics.deltaCount += 2;
            }

            prevL = l;
            prevR = r;
            hasPrev = true;
        }
    };

    struct PatternCase
    {
        int patternIndex = 1;
        float rateHz = 1.0f;
        float depth = 50.0f;
    };

    const std::array<PatternCase, 3> cases{
        PatternCase{1, 1.25f, 60.0f},  // Orbit
        PatternCase{2, 2.50f, 75.0f},  // Pendulum
        PatternCase{3, 4.00f, 85.0f}   // Figure8
    };

    std::array<MotionMetrics, 3> modeMetrics{};
    float prevL = 0.0f;
    float prevR = 0.0f;
    bool hasPrev = false;

    for (size_t c = 0; c < cases.size(); ++c)
    {
        const auto& mc = cases[c];
        setChoice(motionPattern, mc.patternIndex);
        setFloat(motionRate, mc.rateHz);
        setFloat(motionDepth, mc.depth);

        // Let parameter smoothers settle for the mode before scoring.
        for (int warm = 0; warm < 4; ++warm)
            processAndMeasure(modeMetrics[c], prevL, prevR, hasPrev, warm - 4);

        for (int b = 0; b < blocksPerMode; ++b)
            processAndMeasure(modeMetrics[c], prevL, prevR, hasPrev, b);
    }

    // Tempo-sync sweep branch (Orbit pattern), including note division changes.
    MotionMetrics tempoSyncMetrics{};
    setChoice(motionPattern, 1);  // Orbit
    setBool(motionSync, true);
    setFloat(motionDepth, 90.0f);
    const std::array<int, 4> divisionSweep{2, 9, 14, 4};  // 1/4, 1/8T, 1/8D, 1/16
    for (int b = 0; b < blocksPerMode; ++b)
    {
        if ((b % 8) == 0)
            setChoice(motionDivision,
                      divisionSweep[static_cast<size_t>((b / 8) % static_cast<int>(divisionSweep.size()))]);
        processAndMeasure(tempoSyncMetrics, prevL, prevR, hasPrev, b);
    }
    setBool(motionSync, false);

    auto evaluateMetrics = [](const MotionMetrics& m)
    {
        const float rms = (m.sampleCount > 0) ? std::sqrt(static_cast<float>(m.sumSq / m.sampleCount)) : 0.0f;
        const float meanDelta = (m.deltaCount > 0) ? static_cast<float>(m.sumDelta / m.deltaCount) : 0.0f;
        const float spikeRatio = (meanDelta > 1.0e-6f) ? (m.maxDelta / meanDelta) : 0.0f;
        const float spikeThreshold = juce::jmax(0.9f, 6.0f * rms);
        const bool passed = m.finite && m.signal && (m.maxDelta < spikeThreshold) && (spikeRatio < 45.0f);
        return std::tuple<float, float, float, float, bool>(rms, meanDelta, spikeRatio, spikeThreshold, passed);
    };

    bool allPassed = true;
    juce::String result;
    result << "SpatialMotionContinuity:";

    for (size_t i = 0; i < modeMetrics.size(); ++i)
    {
        const auto [rms, meanDelta, spikeRatio, spikeThreshold, passed] = evaluateMetrics(modeMetrics[i]);
        allPassed = allPassed && passed;
        result << " mode" << static_cast<int>(i) << "(rms=" << juce::String(rms, 4)
               << " maxD=" << juce::String(modeMetrics[i].maxDelta, 4) << " meanD=" << juce::String(meanDelta, 4)
               << " ratio=" << juce::String(spikeRatio, 1) << " thr=" << juce::String(spikeThreshold, 3)
               << " worst=" << modeMetrics[i].worstBlock << "/" << modeMetrics[i].worstSample
               << " finite=" << (modeMetrics[i].finite ? "y" : "n") << " sig=" << (modeMetrics[i].signal ? "y" : "n")
               << ")";
    }

    const auto [syncRms, syncMeanDelta, syncSpikeRatio, syncSpikeThreshold, syncPassed] =
        evaluateMetrics(tempoSyncMetrics);
    allPassed = allPassed && syncPassed;
    result << " tempoSync(rms=" << juce::String(syncRms, 4) << " maxD=" << juce::String(tempoSyncMetrics.maxDelta, 4)
           << " meanD=" << juce::String(syncMeanDelta, 4) << " ratio=" << juce::String(syncSpikeRatio, 1)
           << " thr=" << juce::String(syncSpikeThreshold, 3) << " worst=" << tempoSyncMetrics.worstBlock << "/"
           << tempoSyncMetrics.worstSample << " finite=" << (tempoSyncMetrics.finite ? "y" : "n")
           << " sig=" << (tempoSyncMetrics.signal ? "y" : "n") << ") " << (allPassed ? "[PASS]" : "[FAIL]");

    return result;
}

juce::String runSpatialMotionFeatureCoverageTest(SpatialEchoAudioProcessor& processor)
{
    constexpr double fs = 48000.0;
    constexpr int blockSize = 128;
    constexpr float multiplierTolerance = 1.0e-6f;

    processor.prepareToPlay(fs, blockSize);
    auto& vts = processor.getValueTreeState();

    auto* motionPattern = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter("SpatialPattern"));
    auto* motionRate = dynamic_cast<juce::AudioParameterFloat*>(vts.getParameter("SpatialRate"));
    auto* motionSync = dynamic_cast<juce::AudioParameterBool*>(vts.getParameter("SpatialSync"));
    auto* motionDivision = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter("SpatialDivision"));
    auto* motionLongDivision = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter("SpatialLongDivision"));
    auto* motionLongVariantDivision =
        dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter("SpatialLongVariantDivision"));
    auto* motionDepth = dynamic_cast<juce::AudioParameterFloat*>(vts.getParameter("SpatialDepth"));
    auto* motionRetrig = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter("SpatialRestart"));
    auto* motionChoreo = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter("SpatialChoreography"));
    auto* motionSpread = dynamic_cast<juce::AudioParameterFloat*>(vts.getParameter("SpatialSpread"));

    if (motionPattern == nullptr || motionRate == nullptr || motionSync == nullptr || motionDivision == nullptr ||
        motionLongDivision == nullptr || motionLongVariantDivision == nullptr || motionDepth == nullptr ||
        motionRetrig == nullptr || motionChoreo == nullptr || motionSpread == nullptr)
    {
        return "SpatialMotionFeatureCoverage: required parameters missing [FAIL]";
    }

    auto setBool = [](juce::AudioParameterBool* p, bool v) { p->setValueNotifyingHost(v ? 1.0f : 0.0f); };
    auto setFloat = [](juce::AudioParameterFloat* p, float v) { p->setValueNotifyingHost(p->convertTo0to1(v)); };
    auto setChoice = [](juce::AudioParameterChoice* p, int index)
    {
        const int maxIndex = juce::jmax(0, p->choices.size() - 1);
        const int clamped = juce::jlimit(0, maxIndex, index);
        const float norm = (maxIndex > 0) ? (static_cast<float>(clamped) / static_cast<float>(maxIndex)) : 0.0f;
        p->setValueNotifyingHost(norm);
    };

    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    auto processBlock = [&](bool noteOn)
    {
        buffer.clear();
        midi.clear();
        if (noteOn)
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, juce::uint8(100)), 0);
        processor.processBlock(buffer, midi);
    };

    // Common baseline (new motion schema).
    setChoice(motionPattern, 1);  // Orbit
    setBool(motionSync, false);
    setChoice(motionRetrig, 0);  // Free
    setFloat(motionRate, 2.0f);
    setFloat(motionDepth, 60.0f);
    setChoice(motionDivision, 3);             // 1/8
    setChoice(motionLongDivision, 0);         // Off
    setChoice(motionLongVariantDivision, 0);  // Off
    setChoice(motionChoreo, 0);               // Unison
    setFloat(motionSpread, 0.0f);

    // The motion feature coverage test verifies that all motion parameters exist,
    // can be set, and don't cause crashes. Detailed behavior is verified by the
    // SpatialMotionSource and SpatialMotionContinuity tests.

    bool allOk = true;

    // Verify MotionPattern (6 choices: Still, Orbit, Pendulum, Figure8, Breathe, Wander).
    const bool patternHasCorrectChoices = (motionPattern->choices.size() == 6);
    for (int i = 0; i < 6; ++i)
    {
        setChoice(motionPattern, i);
        processBlock(false);
    }
    allOk = allOk && patternHasCorrectChoices;

    // Verify MotionRate (float 0.01..8 Hz).
    setFloat(motionRate, 0.5f);
    processBlock(false);
    setFloat(motionRate, 4.0f);
    processBlock(false);

    // Verify MotionSync (bool).
    setBool(motionSync, true);
    processBlock(false);
    setBool(motionSync, false);
    processBlock(false);

    // SpatialDivision remains byte-for-byte compatible with its original
    // 17-choice host-normalized automation domain. Slower menu entries live in
    // a separate append-only parameter so old DAW automation is not remapped.
    const juce::StringArray expectedMotionDivisions{"1/1",  "1/2",  "1/4",  "1/8",   "1/16",  "1/32",
                                                    "1/64", "1/2T", "1/4T", "1/8T",  "1/16T", "1/32T",
                                                    "1/2D", "1/4D", "1/8D", "1/16D", "1/32D"};
    bool divisionHasCorrectChoices = motionDivision->choices == expectedMotionDivisions;
    divisionHasCorrectChoices =
        divisionHasCorrectChoices && SpatialEchoAudioProcessor::getMotionNoteDivisionCount() == 29;

    const juce::StringArray expectedLongDivisions{"Off", "2/1", "4/1", "8/1", "16/1"};
    const juce::StringArray expectedLongVariantDivisions{"Off",  "2/1T", "4/1T", "8/1T", "16/1T",
                                                         "2/1D", "4/1D", "8/1D", "16/1D"};
    divisionHasCorrectChoices =
        divisionHasCorrectChoices && motionLongDivision->choices == expectedLongDivisions &&
        motionLongVariantDivision->choices == expectedLongVariantDivisions && motionDivision->getVersionHint() == 1 &&
        motionLongDivision->getVersionHint() == 2 && motionLongVariantDivision->getVersionHint() == 3 &&
        motionDivision->isAutomatable() && motionLongDivision->isAutomatable() &&
        motionLongVariantDivision->isAutomatable() &&
        std::abs(static_cast<juce::AudioProcessorParameter*>(motionDivision)->getDefaultValue() -
                 static_cast<float>(SpatialIDs::motion::kDivisionDefault) / 16.0f) <= multiplierTolerance &&
        std::abs(static_cast<juce::AudioProcessorParameter*>(motionLongDivision)->getDefaultValue()) <=
            multiplierTolerance &&
        std::abs(static_cast<juce::AudioProcessorParameter*>(motionLongVariantDivision)->getDefaultValue()) <=
            multiplierTolerance;

    for (int i = 0; i < expectedMotionDivisions.size(); ++i)
    {
        const float legacyNormalized = static_cast<float>(i) / static_cast<float>(expectedMotionDivisions.size() - 1);
        divisionHasCorrectChoices =
            divisionHasCorrectChoices &&
            std::abs(motionDivision->convertTo0to1(static_cast<float>(i)) - legacyNormalized) <= multiplierTolerance;
        motionDivision->setValueNotifyingHost(legacyNormalized);
        divisionHasCorrectChoices = divisionHasCorrectChoices && motionDivision->getIndex() == i;
    }

    for (int i = 0; i < expectedLongDivisions.size(); ++i)
    {
        const float normalized = static_cast<float>(i) / static_cast<float>(expectedLongDivisions.size() - 1);
        divisionHasCorrectChoices =
            divisionHasCorrectChoices &&
            std::abs(motionLongDivision->convertTo0to1(static_cast<float>(i)) - normalized) <= multiplierTolerance;
    }

    for (int i = 0; i < expectedLongVariantDivisions.size(); ++i)
    {
        const float normalized = static_cast<float>(i) / static_cast<float>(expectedLongVariantDivisions.size() - 1);
        divisionHasCorrectChoices =
            divisionHasCorrectChoices && std::abs(motionLongVariantDivision->convertTo0to1(static_cast<float>(i)) -
                                                  normalized) <= multiplierTolerance;
    }

    const juce::StringArray expectedDelayDivisions{"1/1",  "1/2",  "1/4",  "1/8",   "1/16",  "1/32",
                                                   "1/64", "1/2T", "1/4T", "1/8T",  "1/16T", "1/32T",
                                                   "1/2D", "1/4D", "1/8D", "1/16D", "1/32D"};
    for (const auto* delayDivisionId : {"Head1NoteDivision", "Head2NoteDivision", "Head3NoteDivision"})
    {
        const auto* delayDivision = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter(delayDivisionId));
        divisionHasCorrectChoices =
            divisionHasCorrectChoices && delayDivision != nullptr && delayDivision->choices == expectedDelayDivisions;
    }

    constexpr std::array<float, 12> expectedSlowMultipliers{
        8.0f, 16.0f, 32.0f, 64.0f, 5.3333335f, 10.666667f, 21.333334f, 42.666668f, 12.0f, 24.0f, 48.0f, 96.0f};
    for (size_t i = 0; i < expectedSlowMultipliers.size(); ++i)
    {
        const auto actual = SpatialEchoAudioProcessor::getMotionNoteDivisionMultiplier(17 + static_cast<int>(i));
        divisionHasCorrectChoices =
            divisionHasCorrectChoices && std::abs(actual - expectedSlowMultipliers[i]) <= multiplierTolerance;
    }

    for (int i = 0; i < expectedMotionDivisions.size(); ++i)
    {
        setChoice(motionDivision, i);
        processBlock(false);
    }

    setChoice(motionPattern, 1);  // Orbit
    setBool(motionSync, true);
    setChoice(motionLongVariantDivision, 0);
    for (size_t i = 0; i < 4; ++i)
    {
        setChoice(motionLongDivision, 1 + static_cast<int>(i));
        processBlock(false);
        const float expectedRateHz = 2.0f / expectedSlowMultipliers[i];  // 120 BPM
        divisionHasCorrectChoices = divisionHasCorrectChoices && std::abs(processor.getSpatialMotionPreviewRateHz() -
                                                                          expectedRateHz) <= multiplierTolerance;
    }
    for (size_t i = 0; i < 8; ++i)
    {
        // Keep 16/1 active underneath to prove the variant override wins.
        setChoice(motionLongVariantDivision, 1 + static_cast<int>(i));
        processBlock(false);
        const float expectedRateHz = 2.0f / expectedSlowMultipliers[4 + i];
        divisionHasCorrectChoices = divisionHasCorrectChoices && std::abs(processor.getSpatialMotionPreviewRateHz() -
                                                                          expectedRateHz) <= multiplierTolerance;
    }

    struct FixedTempoPlayHead final : juce::AudioPlayHead
    {
        double bpm = 120.0;

        juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override
        {
            juce::AudioPlayHead::PositionInfo info;
            info.setBpm(bpm);
            info.setIsPlaying(true);
            info.setPpqPosition(0.0);
            return info;
        }
    } tempoPlayHead;

    processor.setPlayHead(&tempoPlayHead);
    setChoice(motionLongDivision, 0);
    setChoice(motionLongVariantDivision, 0);
    setChoice(motionDivision, 6);  // 1/64 reaches the established sync ceiling
    processBlock(false);
    divisionHasCorrectChoices =
        divisionHasCorrectChoices && std::abs(processor.getSpatialMotionPreviewRateHz() -
                                              SpatialIDs::motion::kSyncedRateMaxHz) <= multiplierTolerance;

    tempoPlayHead.bpm = 4.0;
    setChoice(motionLongVariantDivision, 8);  // 16/1D = 96 quarter notes per cycle
    processBlock(false);
    constexpr float expectedSlowRateHz = (4.0f / 60.0f) / 96.0f;
    divisionHasCorrectChoices = divisionHasCorrectChoices && std::abs(processor.getSpatialMotionPreviewRateHz() -
                                                                      expectedSlowRateHz) <= multiplierTolerance;
    processor.setPlayHead(nullptr);

    // The added visual rows must not shift horizontal keyboard navigation for
    // the original grid: 1/1 <-> first Triplet stays aligned as before.
    DivisionMenuGrid motionGrid(DivisionMenuGrid::Layout::SpatialMotion);
    int keyboardSelected = -1;
    motionGrid.onItemSelected = [&keyboardSelected](int index) { keyboardSelected = index; };
    motionGrid.setCurrentIndex(0);
    motionGrid.keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
    motionGrid.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    divisionHasCorrectChoices = divisionHasCorrectChoices && keyboardSelected == 7;

    keyboardSelected = -1;
    motionGrid.setCurrentIndex(7);
    motionGrid.keyPressed(juce::KeyPress(juce::KeyPress::leftKey));
    motionGrid.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    divisionHasCorrectChoices = divisionHasCorrectChoices && keyboardSelected == 0;

    const std::array<std::pair<int, const char*>, 12> expectedSlowMenuItems{{{20, "16/1"},
                                                                             {19, "8/1"},
                                                                             {18, "4/1"},
                                                                             {17, "2/1"},
                                                                             {24, "16/1T"},
                                                                             {23, "8/1T"},
                                                                             {22, "4/1T"},
                                                                             {21, "2/1T"},
                                                                             {28, "16/1D"},
                                                                             {27, "8/1D"},
                                                                             {26, "4/1D"},
                                                                             {25, "2/1D"}}};
    DivisionMenuGrid delayGrid(DivisionMenuGrid::Layout::Delay);
    for (const auto& [index, label] : expectedSlowMenuItems)
    {
        divisionHasCorrectChoices = divisionHasCorrectChoices &&
                                    motionGrid.getLabelForMenuIndexForTesting(index) == label &&
                                    delayGrid.getLabelForMenuIndexForTesting(index).isEmpty();
    }

    // Parameter listeners are notified before APVTS refreshes its raw atomic
    // mirror. The displayed composite division must follow the live choice
    // objects immediately, or the DSP can run at 16/1 while the UI stays at
    // the stale 1/8 value after the editor is hidden and shown again.
    auto displayedDivisionIndex = [](const SpatialMotionSection& section)
    {
        for (int i = 0; i < section.getNumChildComponents(); ++i)
        {
            const auto* combo = dynamic_cast<const juce::ComboBox*>(section.getChildComponent(i));
            if (combo != nullptr && combo->getTitle() == "Spatial Division")
                return combo->getSelectedId() - 1;
        }
        return -1;
    };

    setChoice(motionDivision, 3);             // stale value: 1/8
    setChoice(motionLongDivision, 0);         // no long override
    setChoice(motionLongVariantDivision, 0);  // no variant override
    {
        SpatialMotionSection motionSection(vts);
        setChoice(motionLongDivision, 4);  // external/popup write: 16/1
        const bool liveDisplayMatchesDsp =
            motionLongDivision->getIndex() == 4 && displayedDivisionIndex(motionSection) == 20;
        motionSection.setVisible(false);
        motionSection.setVisible(true);
        divisionHasCorrectChoices =
            divisionHasCorrectChoices && liveDisplayMatchesDsp && displayedDivisionIndex(motionSection) == 20;
    }
    {
        SpatialMotionSection reopenedMotionSection(vts);
        divisionHasCorrectChoices = divisionHasCorrectChoices && displayedDivisionIndex(reopenedMotionSection) == 20;
    }

    // Closed ComboBox keyboard changes fire onChange instead of opening the
    // custom grid. They must update the same three-parameter composite state.
    {
        SpatialMotionSection motionSection(vts);
        motionSection.selectDivisionFromClosedComboForTesting(17);  // 2/1
        divisionHasCorrectChoices = divisionHasCorrectChoices && motionLongDivision->getIndex() == 1 &&
                                    motionLongVariantDivision->getIndex() == 0 &&
                                    displayedDivisionIndex(motionSection) == 17;
        motionSection.selectDivisionFromClosedComboForTesting(24);  // 16/1T
        divisionHasCorrectChoices = divisionHasCorrectChoices && motionLongDivision->getIndex() == 0 &&
                                    motionLongVariantDivision->getIndex() == 4 &&
                                    displayedDivisionIndex(motionSection) == 24;
        motionSection.selectDivisionFromClosedComboForTesting(28);  // 16/1D
        divisionHasCorrectChoices = divisionHasCorrectChoices && motionLongDivision->getIndex() == 0 &&
                                    motionLongVariantDivision->getIndex() == 8 &&
                                    displayedDivisionIndex(motionSection) == 28;
        motionSection.selectDivisionFromClosedComboForTesting(2);  // 1/4
        divisionHasCorrectChoices = divisionHasCorrectChoices && motionDivision->getIndex() == 2 &&
                                    motionLongDivision->getIndex() == 0 && motionLongVariantDivision->getIndex() == 0 &&
                                    displayedDivisionIndex(motionSection) == 2;
    }

    setChoice(motionDivision, 6);
    setChoice(motionLongDivision, 4);
    setChoice(motionLongVariantDivision, 8);

    // Older states that omit either additive override must materialize Off
    // before replaceState, or a previously used processor inherits stale
    // long timing that was never present in the saved project.
    juce::MemoryBlock stateWithLongDivisions;
    processor.getStateInformation(stateWithLongDivisions);
    auto removeParameterFromXml = [](juce::XmlElement& xml, const char* parameterId)
    {
        for (auto* child : xml.getChildIterator())
        {
            if (child->hasTagName("PARAM") && child->getStringAttribute("id") == parameterId)
            {
                xml.removeChildElement(child, true);
                return true;
            }
        }
        return false;
    };

    bool olderStateDefaultsVariantDivision = false;
    if (auto v2Xml = juce::AudioProcessor::getXmlFromBinary(stateWithLongDivisions.getData(),
                                                            static_cast<int>(stateWithLongDivisions.getSize())))
    {
        const bool removed = removeParameterFromXml(*v2Xml, "SpatialLongVariantDivision");
        juce::MemoryBlock v2State;
        juce::AudioProcessor::copyXmlToBinary(*v2Xml, v2State);
        setChoice(motionLongVariantDivision, 8);
        processor.setStateInformation(v2State.getData(), static_cast<int>(v2State.getSize()));
        olderStateDefaultsVariantDivision = removed && motionLongVariantDivision->getIndex() == 0 &&
                                            motionLongDivision->getIndex() == 4 && motionDivision->getIndex() == 6;
    }

    bool legacyStateDefaultsLongDivisions = false;
    if (auto v1Xml = juce::AudioProcessor::getXmlFromBinary(stateWithLongDivisions.getData(),
                                                            static_cast<int>(stateWithLongDivisions.getSize())))
    {
        const bool removedLong = removeParameterFromXml(*v1Xml, SpatialIDs::motion::kLongDivision);
        const bool removedVariant = removeParameterFromXml(*v1Xml, "SpatialLongVariantDivision");
        juce::MemoryBlock v1State;
        juce::AudioProcessor::copyXmlToBinary(*v1Xml, v1State);
        setChoice(motionLongDivision, 4);
        setChoice(motionLongVariantDivision, 8);
        processor.setStateInformation(v1State.getData(), static_cast<int>(v1State.getSize()));
        legacyStateDefaultsLongDivisions = removedLong && removedVariant && motionLongDivision->getIndex() == 0 &&
                                           motionLongVariantDivision->getIndex() == 0 &&
                                           motionDivision->getIndex() == 6;
    }
    divisionHasCorrectChoices =
        divisionHasCorrectChoices && olderStateDefaultsVariantDivision && legacyStateDefaultsLongDivisions;

    // Existing spatial scenes explicitly clear the additive override, and a
    // synced scene must not appear matched while a long division is active.
    constexpr int syncedSpatialPresetIndex = 3;  // Low-End Half-Note Orbit
    bool presetLongDivisionContract =
        SpatialPresetCatalog::applyPresetToParameters(vts, syncedSpatialPresetIndex, false);
    setChoice(motionLongDivision, 4);
    presetLongDivisionContract = presetLongDivisionContract && SpatialPresetCatalog::findMatchingPresetIndex(
                                                                   SpatialPresetCatalog::readSnapshot(vts)) == -1;
    presetLongDivisionContract = presetLongDivisionContract &&
                                 SpatialPresetCatalog::applyPresetToParameters(vts, syncedSpatialPresetIndex, false) &&
                                 motionLongDivision->getIndex() == SpatialIDs::motion::kLongDivisionDefault &&
                                 SpatialPresetCatalog::findMatchingPresetIndex(
                                     SpatialPresetCatalog::readSnapshot(vts)) == syncedSpatialPresetIndex;
    setChoice(motionLongVariantDivision, 8);
    presetLongDivisionContract =
        presetLongDivisionContract &&
        SpatialPresetCatalog::findMatchingPresetIndex(SpatialPresetCatalog::readSnapshot(vts)) == -1 &&
        SpatialPresetCatalog::applyPresetToParameters(vts, syncedSpatialPresetIndex, false) &&
        motionLongVariantDivision->getIndex() == 0 &&
        SpatialPresetCatalog::findMatchingPresetIndex(SpatialPresetCatalog::readSnapshot(vts)) ==
            syncedSpatialPresetIndex;
    divisionHasCorrectChoices = divisionHasCorrectChoices && presetLongDivisionContract;

    allOk = allOk && divisionHasCorrectChoices;

    // Verify MotionDepth (float 0..100).
    setFloat(motionDepth, 0.0f);
    processBlock(false);
    setFloat(motionDepth, 50.0f);
    processBlock(false);
    setFloat(motionDepth, 100.0f);
    processBlock(false);

    // Verify MotionRetrig (4 choices: Free, Transport, Note, Random).
    const bool retrigHasCorrectChoices = (motionRetrig->choices.size() == 4);
    for (int i = 0; i < 4; ++i)
    {
        setChoice(motionRetrig, i);
        processBlock(false);
    }
    allOk = allOk && retrigHasCorrectChoices;

    // Verify MotionChoreo (3 choices: Unison, Constellation, Cascade).
    const bool choreoHasCorrectChoices = (motionChoreo->choices.size() == 3);
    for (int i = 0; i < 3; ++i)
    {
        setChoice(motionChoreo, i);
        processBlock(false);
    }
    allOk = allOk && choreoHasCorrectChoices;

    // Verify MotionSpread (float 0..100).
    setFloat(motionSpread, 0.0f);
    processBlock(false);
    setFloat(motionSpread, 50.0f);
    processBlock(false);
    setFloat(motionSpread, 100.0f);
    processBlock(false);

    return allOk ? "SpatialMotionFeatureCoverage: [PASS]" : "SpatialMotionFeatureCoverage: [FAIL]";
}

//==============================================================================
// SPATIAL ROUTING TESTS
// Verifies the spatial signal chain routing feature (Pre/Post-Effects modes).
// Test perspective table
//==============================================================================

juce::String runSpatialRoutingDifferenceTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // Given: Two EchoesModel instances configured identically except for SpatialRoutingMode.
    //        Binaural enabled with azimuth=90 (hard right), delay + reverb active, 440 Hz input.
    // When:  Both models process the same tone for ~640ms.
    // Then:  Outputs must be measurably different (routing changes where HRTF is applied),
    //        and both must produce non-silent output.

    const float fs = 48000.0f;
    const int blockSize = 512;
    const int settleBlocks = 80;
    const int captureBlocks = 60;

    // Helper: call UpdateParameters with common settings and given spatial mode
    auto updateModel = [](EchoesModel& model, int spatialMode)
    {
        int heads[3] = {1, 1, 0};
        ReverbMacroParams reverbMacros;
        reverbMacros.size = 0.5f;
        reverbMacros.decay = 0.5f;
        reverbMacros.tone = juce::jlimit(0.0f, 1.0f, 1.0f - 0.3f / 1.128f);
        reverbMacros.character = (0.7f - 0.15f) / 0.70f;  // cloud diffusion mapping
        reverbMacros.mod = 0.08f;
        reverbMacros.preDelay = 0.0f;
        reverbMacros.width = 0.7f;
        reverbMacros.engine = 0;
        model.UpdateParameters(0.5f, 0.5f, 0.5f, 0.5f, heads, 1, 0.8f, 1, 0.5f, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.5f,
                               0.5f, 0, false, 0.0f, 0.0f, 0.0f, true, 90.0f, 0.0f, 1.0f, 100.0f, 150.0f, 2, 0.5f, 0.5f,
                               0.5f, 0, 0, -1.0f, -1.0f, 1.0f, 1.0f, true, spatialMode, 1.0f, 1.0f, 0.0f, reverbMacros);
    };

    EchoesModel modelPost;
    EchoesModel modelPre;
    modelPost.Reset(fs, 2, blockSize);
    modelPre.Reset(fs, 2, blockSize);
    modelPost.getSpatialField()->getVoiceProcessor(0)->useDefaultHRTF();
    modelPre.getSpatialField()->getVoiceProcessor(0)->useDefaultHRTF();

    std::vector<std::vector<float>> bufPost(2, std::vector<float>(blockSize, 0.0f));
    std::vector<std::vector<float>> bufPre(2, std::vector<float>(blockSize, 0.0f));

    // Settle with tone input (not silence) so delay lines fill with echoes.
    // UpdateParameters called every block to match real processor behavior.
    const float phaseInc = 2.0f * juce::MathConstants<float>::pi * 440.0f / fs;
    float phase = 0.0f;

    for (int b = 0; b < settleBlocks; ++b)
    {
        updateModel(modelPost, 0);
        updateModel(modelPre, 1);

        for (int i = 0; i < blockSize; ++i)
        {
            float sample = 0.5f * std::sin(phase);
            phase += phaseInc;
            if (phase >= 2.0f * juce::MathConstants<float>::pi)
                phase -= 2.0f * juce::MathConstants<float>::pi;
            bufPost[0][static_cast<size_t>(i)] = sample;
            bufPost[1][static_cast<size_t>(i)] = sample;
            bufPre[0][static_cast<size_t>(i)] = sample;
            bufPre[1][static_cast<size_t>(i)] = sample;
        }
        modelPost.ProcessBuffer(bufPost, blockSize);
        modelPre.ProcessBuffer(bufPre, blockSize);
    }

    // Capture phase
    std::vector<float> postL, postR, preL, preR;
    postL.reserve(static_cast<size_t>(captureBlocks * blockSize));
    postR.reserve(static_cast<size_t>(captureBlocks * blockSize));
    preL.reserve(static_cast<size_t>(captureBlocks * blockSize));
    preR.reserve(static_cast<size_t>(captureBlocks * blockSize));

    for (int b = 0; b < captureBlocks; ++b)
    {
        updateModel(modelPost, 0);
        updateModel(modelPre, 1);

        for (int i = 0; i < blockSize; ++i)
        {
            float sample = 0.5f * std::sin(phase);
            phase += phaseInc;
            if (phase >= 2.0f * juce::MathConstants<float>::pi)
                phase -= 2.0f * juce::MathConstants<float>::pi;
            bufPost[0][static_cast<size_t>(i)] = sample;
            bufPost[1][static_cast<size_t>(i)] = sample;
            bufPre[0][static_cast<size_t>(i)] = sample;
            bufPre[1][static_cast<size_t>(i)] = sample;
        }

        modelPost.ProcessBuffer(bufPost, blockSize);
        modelPre.ProcessBuffer(bufPre, blockSize);

        postL.insert(postL.end(), bufPost[0].begin(), bufPost[0].end());
        postR.insert(postR.end(), bufPost[1].begin(), bufPost[1].end());
        preL.insert(preL.end(), bufPre[0].begin(), bufPre[0].end());
        preR.insert(preR.end(), bufPre[1].begin(), bufPre[1].end());
    }

    const int n = static_cast<int>(postL.size());

    double diffSumSq = 0.0;
    double postSumSq = 0.0;
    for (int i = 0; i < n; ++i)
    {
        double d =
            static_cast<double>(postL[static_cast<size_t>(i)]) - static_cast<double>(preL[static_cast<size_t>(i)]);
        diffSumSq += d * d;
        postSumSq +=
            static_cast<double>(postL[static_cast<size_t>(i)]) * static_cast<double>(postL[static_cast<size_t>(i)]);
    }
    const float diffRms = std::sqrt(static_cast<float>(diffSumSq / n));
    const float postRms = std::sqrt(static_cast<float>(postSumSq / n));
    const float diffRatioDb = (postRms > 1.0e-8f) ? 20.0f * std::log10(diffRms / postRms) : -100.0f;
    const bool outputsDiffer = diffRms > 0.001f;

    float postPeak = 0.0f, prePeak = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        postPeak = std::max(postPeak,
                            std::max(std::abs(postL[static_cast<size_t>(i)]), std::abs(postR[static_cast<size_t>(i)])));
        prePeak =
            std::max(prePeak, std::max(std::abs(preL[static_cast<size_t>(i)]), std::abs(preR[static_cast<size_t>(i)])));
    }
    const bool postActive = postPeak > 0.01f;
    const bool preActive = prePeak > 0.01f;

    const bool passed = outputsDiffer && postActive && preActive;

    juce::String result;
    result << "SpatialRoutingDifference: diffRms=" << juce::String(diffRms, 5)
           << " diffRatio=" << juce::String(diffRatioDb, 1) << "dB"
           << " postPeak=" << juce::String(postPeak, 4) << " prePeak=" << juce::String(prePeak, 4) << " "
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runSpatialRoutingBypassEquivalenceTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // Given: Two EchoesModel instances with different SpatialRoutingMode (0 vs 1),
    //        but binaural DISABLED.
    // When:  Both process identical 440 Hz input.
    // Then:  Outputs must be identical (within float precision), proving routing mode
    //        has no effect when binaural is off.

    const float fs = 48000.0f;
    const int blockSize = 512;
    const int settleBlocks = 80;
    const int captureBlocks = 30;

    auto updateModel = [](EchoesModel& model, int spatialMode)
    {
        int heads[3] = {1, 1, 0};
        ReverbMacroParams reverbMacros;
        reverbMacros.size = 0.5f;
        reverbMacros.decay = 0.5f;
        reverbMacros.tone = juce::jlimit(0.0f, 1.0f, 1.0f - 0.3f / 1.128f);
        reverbMacros.character = (0.7f - 0.15f) / 0.70f;  // cloud diffusion mapping
        reverbMacros.mod = 0.08f;
        reverbMacros.preDelay = 0.0f;
        reverbMacros.width = 0.7f;
        reverbMacros.engine = 0;
        model.UpdateParameters(0.5f, 0.5f, 0.5f, 0.5f, heads, 1, 0.7f, 1, 0.5f, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.5f,
                               0.5f, 0, false, 0.0f, 0.0f, 0.0f, false, 0.0f, 0.0f, 1.0f, 100.0f, 150.0f, 2, 0.5f, 0.5f,
                               0.5f, 0, 0, -1.0f, -1.0f, 1.0f, 1.0f, true, spatialMode, 1.0f, 1.0f, 0.0f, reverbMacros);
    };

    EchoesModel modelPost;
    EchoesModel modelPre;
    modelPost.Reset(fs, 2, blockSize);
    modelPre.Reset(fs, 2, blockSize);

    std::vector<std::vector<float>> bufPost(2, std::vector<float>(blockSize, 0.0f));
    std::vector<std::vector<float>> bufPre(2, std::vector<float>(blockSize, 0.0f));

    const float phaseInc = 2.0f * juce::MathConstants<float>::pi * 440.0f / fs;
    float phase = 0.0f;

    // Settle with tone input, UpdateParameters every block
    for (int b = 0; b < settleBlocks; ++b)
    {
        updateModel(modelPost, 0);
        updateModel(modelPre, 1);

        for (int i = 0; i < blockSize; ++i)
        {
            float sample = 0.5f * std::sin(phase);
            phase += phaseInc;
            if (phase >= 2.0f * juce::MathConstants<float>::pi)
                phase -= 2.0f * juce::MathConstants<float>::pi;
            bufPost[0][static_cast<size_t>(i)] = sample;
            bufPost[1][static_cast<size_t>(i)] = sample;
            bufPre[0][static_cast<size_t>(i)] = sample;
            bufPre[1][static_cast<size_t>(i)] = sample;
        }
        modelPost.ProcessBuffer(bufPost, blockSize);
        modelPre.ProcessBuffer(bufPre, blockSize);
    }

    double maxAbsDiff = 0.0;
    float peakOutput = 0.0f;

    for (int b = 0; b < captureBlocks; ++b)
    {
        updateModel(modelPost, 0);
        updateModel(modelPre, 1);

        for (int i = 0; i < blockSize; ++i)
        {
            float sample = 0.5f * std::sin(phase);
            phase += phaseInc;
            if (phase >= 2.0f * juce::MathConstants<float>::pi)
                phase -= 2.0f * juce::MathConstants<float>::pi;
            bufPost[0][static_cast<size_t>(i)] = sample;
            bufPost[1][static_cast<size_t>(i)] = sample;
            bufPre[0][static_cast<size_t>(i)] = sample;
            bufPre[1][static_cast<size_t>(i)] = sample;
        }

        modelPost.ProcessBuffer(bufPost, blockSize);
        modelPre.ProcessBuffer(bufPre, blockSize);

        for (int ch = 0; ch < 2; ++ch)
        {
            for (int i = 0; i < blockSize; ++i)
            {
                double diff = std::abs(static_cast<double>(bufPost[static_cast<size_t>(ch)][static_cast<size_t>(i)]) -
                                       static_cast<double>(bufPre[static_cast<size_t>(ch)][static_cast<size_t>(i)]));
                maxAbsDiff = std::max(maxAbsDiff, diff);
                peakOutput = std::max(peakOutput, std::abs(bufPost[static_cast<size_t>(ch)][static_cast<size_t>(i)]));
            }
        }
    }

    const bool identical = maxAbsDiff < 1.0e-5;
    const bool hasOutput = peakOutput > 0.01f;
    const bool passed = identical && hasOutput;

    juce::String result;
    result << "SpatialRoutingBypassEquiv: maxDiff=" << juce::String(maxAbsDiff, 8)
           << " peakOut=" << juce::String(peakOutput, 4) << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runSpatialRoutingLatencyContractTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr float fs = 48000.0f;
    constexpr int blockSize = 128;
    constexpr int settleBlocks = 48;
    constexpr int renderBlocks = 120;
    constexpr int dryWindowEnd = static_cast<int>(0.015f * fs);
    constexpr int echoWindowStart = static_cast<int>(0.060f * fs);
    constexpr int echoWindowEnd = static_cast<int>(0.125f * fs);
    constexpr float dryCentroidLimitMs = 6.0f;
    constexpr float echoCentroidLimitMs = 6.0f;
    constexpr float minDryPeak = 0.015f;
    constexpr float minEchoPeak = 0.002f;

    struct WindowMetrics
    {
        float peak = 0.0f;
        float centroidSamples = 0.0f;
        bool hasSignal = false;
    };

    struct RenderMetrics
    {
        WindowMetrics dry;
        WindowMetrics echo;
        float peak = 0.0f;
        bool finite = true;
    };

    auto updateModel = [](EchoesModel& model, int spatialMode, float mix, float binauralWetMix)
    {
        int heads[3] = {1, 0, 0};
        ReverbMacroParams reverbMacros;
        reverbMacros.size = 0.5f;
        reverbMacros.decay = 0.5f;
        reverbMacros.tone = juce::jlimit(0.0f, 1.0f, 1.0f - 0.3f / 1.128f);
        reverbMacros.character = (0.7f - 0.15f) / 0.70f;  // cloud diffusion mapping
        reverbMacros.mod = 0.08f;
        reverbMacros.preDelay = 0.0f;
        reverbMacros.width = 0.7f;
        reverbMacros.engine = 0;
        model.UpdateParameters(0.5f, 0.5f, 0.0f, 0.5f, heads, 1, mix, 0, 0.5f, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.5f,
                               0.5f, 0, true, 80.0f, 160.0f, 240.0f, true, 90.0f, 0.0f, 1.0f, binauralWetMix, 150.0f, 2,
                               0.2f, 0.2f, 0.5f, 0, 0, -1.0f, -1.0f, 1.0f, 1.0f, true, spatialMode, 1.0f, 0.0f, 0.0f,
                               reverbMacros);
    };

    auto measureWindow = [](const std::vector<float>& left, const std::vector<float>& right, int start, int end,
                            float minPeak) -> WindowMetrics
    {
        WindowMetrics metrics;
        const int clampedStart = juce::jlimit(0, static_cast<int>(left.size()), start);
        const int clampedEnd = juce::jlimit(clampedStart, static_cast<int>(left.size()), end);
        double weightedIndex = 0.0;
        double totalWeight = 0.0;

        for (int i = clampedStart; i < clampedEnd; ++i)
        {
            const float mag =
                juce::jmax(std::abs(left[static_cast<size_t>(i)]), std::abs(right[static_cast<size_t>(i)]));
            metrics.peak = juce::jmax(metrics.peak, mag);
            weightedIndex += static_cast<double>(i) * static_cast<double>(mag);
            totalWeight += static_cast<double>(mag);
        }

        metrics.hasSignal = metrics.peak >= minPeak && totalWeight > 1.0e-9;
        metrics.centroidSamples = (totalWeight > 1.0e-9) ? static_cast<float>(weightedIndex / totalWeight) : 0.0f;
        return metrics;
    };

    auto renderCase = [&](int spatialMode, float mix, float binauralWetMix) -> RenderMetrics
    {
        EchoesModel model;
        model.Reset(fs, 2, blockSize);
        model.getSpatialField()->getVoiceProcessor(0)->useDefaultHRTF();

        std::vector<std::vector<float>> buffer(2, std::vector<float>(blockSize, 0.0f));

        for (int b = 0; b < settleBlocks; ++b)
        {
            updateModel(model, spatialMode, mix, binauralWetMix);
            for (auto& channel : buffer)
                std::fill(channel.begin(), channel.end(), 0.0f);
            model.ProcessBuffer(buffer, blockSize);
        }

        std::vector<float> left;
        std::vector<float> right;
        left.reserve(static_cast<size_t>(renderBlocks * blockSize));
        right.reserve(static_cast<size_t>(renderBlocks * blockSize));

        for (int b = 0; b < renderBlocks; ++b)
        {
            updateModel(model, spatialMode, mix, binauralWetMix);
            for (auto& channel : buffer)
                std::fill(channel.begin(), channel.end(), 0.0f);

            if (b == 0)
            {
                buffer[0][0] = 0.8f;
                buffer[1][0] = 0.8f;
            }

            model.ProcessBuffer(buffer, blockSize);

            for (int i = 0; i < blockSize; ++i)
            {
                const float l = buffer[0][static_cast<size_t>(i)];
                const float r = buffer[1][static_cast<size_t>(i)];
                left.push_back(l);
                right.push_back(r);
            }
        }

        RenderMetrics metrics;
        for (size_t i = 0; i < left.size(); ++i)
        {
            const float l = left[i];
            const float r = right[i];
            if (!std::isfinite(l) || !std::isfinite(r))
                metrics.finite = false;
            metrics.peak = juce::jmax(metrics.peak, juce::jmax(std::abs(l), std::abs(r)));
        }

        metrics.dry = measureWindow(left, right, 0, dryWindowEnd, minDryPeak);
        metrics.echo = measureWindow(left, right, echoWindowStart, echoWindowEnd, minEchoPeak);
        return metrics;
    };

    int failures = 0;
    float maxDryDeltaMs = 0.0f;
    float maxEchoDeltaMs = 0.0f;
    float minDry = std::numeric_limits<float>::max();
    float minEcho = std::numeric_limits<float>::max();
    float maxPeak = 0.0f;
    bool finite = true;

    const std::array<float, 3> mixes{0.25f, 0.5f, 1.0f};
    const std::array<float, 2> binauralWetMixes{50.0f, 100.0f};

    for (float mix : mixes)
    {
        for (float binauralWetMix : binauralWetMixes)
        {
            const RenderMetrics post = renderCase(0, mix, binauralWetMix);
            const RenderMetrics pre = renderCase(1, mix, binauralWetMix);

            finite = finite && post.finite && pre.finite;
            maxPeak = juce::jmax(maxPeak, juce::jmax(post.peak, pre.peak));
            minEcho = juce::jmin(minEcho, juce::jmin(post.echo.peak, pre.echo.peak));
            if (!post.echo.hasSignal || !pre.echo.hasSignal)
                ++failures;

            const float echoDeltaMs = std::abs(post.echo.centroidSamples - pre.echo.centroidSamples) / fs * 1000.0f;
            maxEchoDeltaMs = juce::jmax(maxEchoDeltaMs, echoDeltaMs);
            if (echoDeltaMs > echoCentroidLimitMs)
                ++failures;

            if (mix < 0.999f)
            {
                minDry = juce::jmin(minDry, juce::jmin(post.dry.peak, pre.dry.peak));
                if (!post.dry.hasSignal || !pre.dry.hasSignal)
                    ++failures;

                const float dryDeltaMs = std::abs(post.dry.centroidSamples - pre.dry.centroidSamples) / fs * 1000.0f;
                maxDryDeltaMs = juce::jmax(maxDryDeltaMs, dryDeltaMs);
                if (dryDeltaMs > dryCentroidLimitMs)
                    ++failures;
            }
        }
    }

    if (!finite || maxPeak > 1.25f)
        ++failures;

    const bool passed = failures == 0;

    juce::String result;
    result << "SpatialRoutingLatencyContract:"
           << " cases=" << static_cast<int>(mixes.size() * binauralWetMixes.size()) << " failures=" << failures
           << " dryDelta=" << juce::String(maxDryDeltaMs, 2) << "ms"
           << " echoDelta=" << juce::String(maxEchoDeltaMs, 2) << "ms"
           << " minDry="
           << juce::String(juce::exactlyEqual(minDry, std::numeric_limits<float>::max()) ? 0.0f : minDry, 4)
           << " minEcho=" << juce::String(minEcho, 4) << " peak=" << juce::String(maxPeak, 4)
           << " finite=" << (finite ? "yes" : "no") << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runSpatialRoutingBinauralImageTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr float fs = 48000.0f;
    constexpr int blockSize = 256;
    constexpr int settleBlocks = 40;
    constexpr int captureBlocks = 48;
    constexpr float inputAmp = 0.25f;
    constexpr float inputHz = 1000.0f;

    struct ImageMetrics
    {
        float ildDb = 0.0f;
        float peak = 0.0f;
        bool finite = true;
    };

    auto renderCase = [&](int spatialMode, float azimuth, int settleBlockCount) -> ImageMetrics
    {
        EchoesModel model;
        model.Reset(fs, 2, blockSize);
        model.getSpatialField()->getVoiceProcessor(0)->useDefaultHRTF();
        model.getSpatialField()->getVoiceProcessor(0)->setDebugDisableEarlyReflections(true);
        model.getSpatialField()->getVoiceProcessor(0)->setDebugDisableAirAbsorption(true);
        model.getSpatialField()->getVoiceProcessor(0)->setDebugDisableWidth(true);
        model.getSpatialField()->getVoiceProcessor(0)->setDebugDisableDiffuseEQ(true);

        auto updateModel = [&]()
        {
            int heads[3] = {1, 0, 0};
            model.UpdateParameters(0.5f, 0.5f, 0.0f, 0.5f, heads, 1, 1.0f, 0, 0.65f, 1.0f, false, 0.0f, 0.0f, 0.0f,
                                   0.5f, 0.5f, 0, true, 80.0f, 160.0f, 240.0f, true, azimuth, 0.0f, 1.0f, 100.0f, 20.0f,
                                   2, 0.25f, 0.25f, 0.5f, 0, 0, -1.0f, -1.0f, 1.0f, 1.0f, true, spatialMode, 1.0f,
                                   0.0f);
        };

        std::vector<std::vector<float>> buffer(2, std::vector<float>(static_cast<size_t>(blockSize), 0.0f));
        float phase = 0.0f;
        const float phaseInc = 2.0f * juce::MathConstants<float>::pi * inputHz / fs;

        auto fillTone = [&]()
        {
            for (int i = 0; i < blockSize; ++i)
            {
                const float sample = inputAmp * std::sin(phase);
                phase += phaseInc;
                if (phase >= 2.0f * juce::MathConstants<float>::pi)
                    phase -= 2.0f * juce::MathConstants<float>::pi;
                buffer[0][static_cast<size_t>(i)] = sample;
                buffer[1][static_cast<size_t>(i)] = sample;
            }
        };

        for (int b = 0; b < settleBlockCount; ++b)
        {
            updateModel();
            fillTone();
            model.ProcessBuffer(buffer, blockSize);
        }

        double leftSq = 0.0;
        double rightSq = 0.0;
        int count = 0;
        ImageMetrics metrics;

        for (int b = 0; b < captureBlocks; ++b)
        {
            updateModel();
            fillTone();
            model.ProcessBuffer(buffer, blockSize);

            for (int i = 0; i < blockSize; ++i)
            {
                const float l = buffer[0][static_cast<size_t>(i)];
                const float r = buffer[1][static_cast<size_t>(i)];
                if (!std::isfinite(l) || !std::isfinite(r))
                    metrics.finite = false;
                metrics.peak = juce::jmax(metrics.peak, juce::jmax(std::abs(l), std::abs(r)));
                leftSq += static_cast<double>(l) * static_cast<double>(l);
                rightSq += static_cast<double>(r) * static_cast<double>(r);
                ++count;
            }
        }

        const float leftRms = (count > 0) ? static_cast<float>(std::sqrt(leftSq / static_cast<double>(count))) : 0.0f;
        const float rightRms = (count > 0) ? static_cast<float>(std::sqrt(rightSq / static_cast<double>(count))) : 0.0f;
        metrics.ildDb = TestUtilities::linearToDb(rightRms / juce::jmax(leftRms, 1.0e-12f));
        return metrics;
    };

    struct BypassMetrics
    {
        std::vector<float> left;
        std::vector<float> right;
        float peak = 0.0f;
        bool finite = true;
    };

    auto renderBypassCase = [&](bool binauralEnabled, float wetMixPercent) -> BypassMetrics
    {
        EchoesModel model;
        model.Reset(fs, 2, blockSize);
        model.getSpatialField()->getVoiceProcessor(0)->useDefaultHRTF();
        model.getSpatialField()->getVoiceProcessor(0)->setDebugDisableEarlyReflections(true);
        model.getSpatialField()->getVoiceProcessor(0)->setDebugDisableAirAbsorption(true);
        model.getSpatialField()->getVoiceProcessor(0)->setDebugDisableWidth(true);
        model.getSpatialField()->getVoiceProcessor(0)->setDebugDisableDiffuseEQ(true);

        auto updateModel = [&]()
        {
            int heads[3] = {1, 0, 0};
            model.UpdateParameters(0.5f, 0.5f, 0.0f, 0.5f, heads, 1, 1.0f, 0, 0.25f, 1.0f, false, 0.0f, 0.0f, 0.0f,
                                   0.5f, 0.5f, 0, true, 80.0f, 160.0f, 240.0f, binauralEnabled, 90.0f, 0.0f, 1.0f,
                                   wetMixPercent, 20.0f, 2, 0.1f, 0.1f, 0.5f, 0, 0, -1.0f, -1.0f, 1.0f, 1.0f, true, 1,
                                   1.0f, 0.0f);
        };

        std::vector<std::vector<float>> buffer(2, std::vector<float>(static_cast<size_t>(blockSize), 0.0f));
        float phase = 0.0f;
        const float phaseInc = 2.0f * juce::MathConstants<float>::pi * inputHz / fs;

        auto fillAntiPhaseTone = [&]()
        {
            for (int i = 0; i < blockSize; ++i)
            {
                const float sample = 0.08f * std::sin(phase);
                phase += phaseInc;
                if (phase >= 2.0f * juce::MathConstants<float>::pi)
                    phase -= 2.0f * juce::MathConstants<float>::pi;
                buffer[0][static_cast<size_t>(i)] = sample;
                buffer[1][static_cast<size_t>(i)] = -sample;
            }
        };

        BypassMetrics metrics;
        metrics.left.reserve(static_cast<size_t>(captureBlocks * blockSize));
        metrics.right.reserve(static_cast<size_t>(captureBlocks * blockSize));
        for (int b = 0; b < settleBlocks + captureBlocks; ++b)
        {
            updateModel();
            fillAntiPhaseTone();
            model.ProcessBuffer(buffer, blockSize);

            if (b >= settleBlocks)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    const float left = buffer[0][static_cast<size_t>(i)];
                    const float right = buffer[1][static_cast<size_t>(i)];
                    metrics.finite = metrics.finite && std::isfinite(left) && std::isfinite(right);
                    metrics.peak = juce::jmax(metrics.peak, juce::jmax(std::abs(left), std::abs(right)));
                    metrics.left.push_back(left);
                    metrics.right.push_back(right);
                }
            }
        }

        return metrics;
    };

    int failures = 0;
    float minAbsIld = std::numeric_limits<float>::max();
    float minColdAbsIld = std::numeric_limits<float>::max();
    float maxPeak = 0.0f;
    bool finite = true;

    std::array<float, 2> postIlds{};
    std::array<float, 2> preIlds{};
    std::array<float, 2> coldPreIlds{};
    const std::array<float, 2> azimuths{90.0f, -90.0f};

    for (size_t i = 0; i < azimuths.size(); ++i)
    {
        const ImageMetrics post = renderCase(0, azimuths[i], settleBlocks);
        const ImageMetrics pre = renderCase(1, azimuths[i], settleBlocks);
        const ImageMetrics coldPre = renderCase(1, azimuths[i], 0);

        postIlds[i] = post.ildDb;
        preIlds[i] = pre.ildDb;
        coldPreIlds[i] = coldPre.ildDb;
        minAbsIld = juce::jmin(minAbsIld, juce::jmin(std::abs(post.ildDb), std::abs(pre.ildDb)));
        minColdAbsIld = juce::jmin(minColdAbsIld, std::abs(coldPre.ildDb));
        maxPeak = juce::jmax(maxPeak, juce::jmax(coldPre.peak, juce::jmax(post.peak, pre.peak)));
        finite = finite && post.finite && pre.finite && coldPre.finite;
    }

    const auto disabledBypass = renderBypassCase(false, 100.0f);
    const auto zeroWetBypass = renderBypassCase(true, 0.0f);
    float bypassMaxDifference = 0.0f;
    const bool bypassSizesMatch = disabledBypass.left.size() == zeroWetBypass.left.size() &&
                                  disabledBypass.right.size() == zeroWetBypass.right.size();
    if (bypassSizesMatch)
    {
        for (size_t i = 0; i < disabledBypass.left.size(); ++i)
        {
            bypassMaxDifference =
                juce::jmax(bypassMaxDifference, std::abs(disabledBypass.left[i] - zeroWetBypass.left[i]));
            bypassMaxDifference =
                juce::jmax(bypassMaxDifference, std::abs(disabledBypass.right[i] - zeroWetBypass.right[i]));
        }
    }

    if (postIlds[0] * postIlds[1] >= 0.0f)
        ++failures;
    if (preIlds[0] * preIlds[1] >= 0.0f)
        ++failures;
    if (coldPreIlds[0] * coldPreIlds[1] >= 0.0f)
        ++failures;
    if (minAbsIld < 1.0f)
        ++failures;
    if (minColdAbsIld < 0.5f)
        ++failures;
    if (!bypassSizesMatch || !disabledBypass.finite || !zeroWetBypass.finite || bypassMaxDifference > 1.0e-6f)
        ++failures;
    if (!finite || maxPeak > 1.25f || maxPeak < 0.001f)
        ++failures;

    const bool passed = failures == 0;
    juce::String result;
    result << "SpatialRoutingBinauralImage:"
           << " post(+/-)=" << juce::String(postIlds[0], 2) << "/" << juce::String(postIlds[1], 2) << "dB"
           << " pre(+/-)=" << juce::String(preIlds[0], 2) << "/" << juce::String(preIlds[1], 2) << "dB"
           << " coldPre(+/-)=" << juce::String(coldPreIlds[0], 2) << "/" << juce::String(coldPreIlds[1], 2) << "dB"
           << " bypassPeak=" << juce::String(disabledBypass.peak, 5) << "/" << juce::String(zeroWetBypass.peak, 5)
           << " bypassDiff=" << juce::String(bypassMaxDifference, 8) << " minAbsIld=" << juce::String(minAbsIld, 2)
           << "dB"
           << " minColdIld=" << juce::String(minColdAbsIld, 2) << "dB"
           << " peak=" << juce::String(maxPeak, 4) << " finite=" << (finite ? "yes" : "no") << " failures=" << failures
           << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

//==============================================================================
// Spatial Metric Battery
// Objective, listening-free measurement of the binaural renderer's spatial cues.
// For a set of azimuths it renders a centred broadband source positioned at that
// azimuth and measures signed ILD (dB) and ITD (microseconds, via L/R cross-
// correlation), and gates the sign and plausibility of those cues.
//==============================================================================
juce::String runSpatialMetricBatteryTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr float fs = 48000.0f;
    constexpr int blockSize = 256;
    constexpr int settleBlocks = 60;
    constexpr int captureBlocks = 48;

    const std::array<float, 7> azimuths{-90.0f, -60.0f, -30.0f, 0.0f, 30.0f, 60.0f, 90.0f};

    struct AngleMetric
    {
        float az = 0.0f;
        float ildDb = 0.0f;  // +ve = right louder
        float itdUs = 0.0f;  // +ve = right ear leads
        float peak = 0.0f;
        bool finite = true;
    };

    // Deterministic broadband noise (LCG) so the measurement is repeatable.
    struct Lcg
    {
        uint32_t state;
        inline float next()
        {
            state = state * 1664525u + 1013904223u;
            return static_cast<float>((state >> 8) & 0xFFFFFFu) / 8388608.0f - 1.0f;
        }
    };

    auto renderAngle = [&](float az) -> AngleMetric
    {
        EchoesModel model;
        model.Reset(fs, 2, blockSize);
        model.getSpatialField()->getVoiceProcessor(0)->useDefaultHRTF();
        // Strip extra coloration so ILD/ITD reflect the HRTF, not width/reflections.
        model.getSpatialField()->getVoiceProcessor(0)->setDebugDisableEarlyReflections(true);
        model.getSpatialField()->getVoiceProcessor(0)->setDebugDisableAirAbsorption(true);
        model.getSpatialField()->getVoiceProcessor(0)->setDebugDisableWidth(true);
        model.getSpatialField()->getVoiceProcessor(0)->setDebugDisableDiffuseEQ(true);

        auto updateModel = [&]()
        {
            int heads[3] = {1, 0, 0};
            model.UpdateParameters(0.5f, 0.5f, 0.0f, 0.5f, heads, 1, 1.0f, 0, 0.65f, 1.0f, false, 0.0f, 0.0f, 0.0f,
                                   0.5f, 0.5f, 0, true, 80.0f, 160.0f, 240.0f, true, az, 0.0f, 1.0f, 100.0f, 150.0f,
                                   2,  // binaural on, az, el=0, dist=1, wet=100, crossover=150, quality=2
                                   0.25f, 0.25f, 0.5f, 0, 0, -1.0f, -1.0f, 1.0f, 1.0f, true,
                                   1,  // SpatialRoutingMode = Before effects: binaural sees the dry source directly
                                   1.0f, 0.0f);
        };

        std::vector<std::vector<float>> buffer(2, std::vector<float>(static_cast<size_t>(blockSize), 0.0f));
        Lcg lcg{0x1234567u};
        auto fillNoise = [&]()
        {
            for (int i = 0; i < blockSize; ++i)
            {
                const float s = 0.25f * lcg.next();
                buffer[0][static_cast<size_t>(i)] = s;
                buffer[1][static_cast<size_t>(i)] = s;
            }
        };

        for (int b = 0; b < settleBlocks; ++b)
        {
            updateModel();
            fillNoise();
            model.ProcessBuffer(buffer, blockSize);
        }

        AngleMetric m;
        m.az = az;
        std::vector<float> capL, capR;
        capL.reserve(static_cast<size_t>(captureBlocks * blockSize));
        capR.reserve(static_cast<size_t>(captureBlocks * blockSize));
        double leftSq = 0.0, rightSq = 0.0;

        for (int b = 0; b < captureBlocks; ++b)
        {
            updateModel();
            fillNoise();
            model.ProcessBuffer(buffer, blockSize);
            for (int i = 0; i < blockSize; ++i)
            {
                const float l = buffer[0][static_cast<size_t>(i)];
                const float r = buffer[1][static_cast<size_t>(i)];
                if (!std::isfinite(l) || !std::isfinite(r))
                    m.finite = false;
                m.peak = juce::jmax(m.peak, juce::jmax(std::abs(l), std::abs(r)));
                leftSq += static_cast<double>(l) * static_cast<double>(l);
                rightSq += static_cast<double>(r) * static_cast<double>(r);
                capL.push_back(l);
                capR.push_back(r);
            }
        }

        const int n = static_cast<int>(capL.size());
        const float leftRms = (n > 0) ? static_cast<float>(std::sqrt(leftSq / n)) : 0.0f;
        const float rightRms = (n > 0) ? static_cast<float>(std::sqrt(rightSq / n)) : 0.0f;
        m.ildDb = TestUtilities::linearToDb(rightRms / juce::jmax(leftRms, 1.0e-12f));

        // ITD via L/R cross-correlation peak. acc(lag)=sum L[k]*R[k+lag].
        // If R leads L by d samples, peak is at lag=-d, so itd(right-leads) = -bestLag.
        const int maxLag = static_cast<int>(0.0013f * fs);  // +-1.3 ms physical window
        double best = -1.0e300;
        int bestLag = 0;
        for (int lag = -maxLag; lag <= maxLag; ++lag)
        {
            double acc = 0.0;
            const int start = juce::jmax(0, -lag);
            const int end = juce::jmin(n, n - lag);
            for (int k = start; k < end; ++k)
                acc += static_cast<double>(capL[static_cast<size_t>(k)]) *
                       static_cast<double>(capR[static_cast<size_t>(k + lag)]);
            if (acc > best)
            {
                best = acc;
                bestLag = lag;
            }
        }
        m.itdUs = -static_cast<float>(bestLag) / fs * 1.0e6f;
        return m;
    };

    std::array<AngleMetric, 7> metrics{};
    for (size_t i = 0; i < azimuths.size(); ++i)
        metrics[i] = renderAngle(azimuths[i]);

    // Summary metrics.
    float maxAbsIld = 0.0f;
    float maxAbsItd = 0.0f;
    float maxPeak = 0.0f;
    bool finite = true;
    AngleMetric* at0 = nullptr;
    AngleMetric* atP90 = nullptr;
    AngleMetric* atM90 = nullptr;
    for (auto& m : metrics)
    {
        maxAbsIld = juce::jmax(maxAbsIld, std::abs(m.ildDb));
        maxAbsItd = juce::jmax(maxAbsItd, std::abs(m.itdUs));
        maxPeak = juce::jmax(maxPeak, m.peak);
        finite = finite && m.finite;
        if (std::abs(m.az) < 0.5f)
            at0 = &m;
        else if (std::abs(m.az - 90.0f) < 0.5f)
            atP90 = &m;
        else if (std::abs(m.az + 90.0f) < 0.5f)
            atM90 = &m;
    }

    const float ild90Sym = (atP90 && atM90) ? (atP90->ildDb * atM90->ildDb) : 1.0f;  // expect < 0
    const float itd90Sym = (atP90 && atM90) ? (atP90->itdUs * atM90->itdUs) : 1.0f;  // expect < 0
    const float ild0 = at0 ? std::abs(at0->ildDb) : 0.0f;                            // expect ~0 (centred)
    const float itd90Mag = (atP90 && atM90) ? 0.5f * (std::abs(atP90->itdUs) + std::abs(atM90->itdUs)) : 0.0f;

    int failures = 0;
    if (!finite)
        ++failures;  // no NaN/Inf
    if (maxPeak < 0.001f || maxPeak > 1.5f)
        ++failures;  // sane output level
    if (maxAbsIld < 2.0f)
        ++failures;  // lateralization present at extremes
    if (ild90Sym >= 0.0f)
        ++failures;  // +90 and -90 ILD opposite sign
    if (ild0 > 3.0f)
        ++failures;  // centred source roughly balanced
    if (itd90Mag < 150.0f || itd90Mag > 1100.0f)
        ++failures;  // ITD at +-90 physically plausible
    if (itd90Sym >= 0.0f)
        ++failures;  // +90 and -90 ITD opposite sign

    const bool passed = failures == 0;

    juce::String result;
    result << "SpatialMetricBattery:";
    for (const auto& m : metrics)
        result << " az" << juce::String(static_cast<int>(m.az)) << "[ild=" << juce::String(m.ildDb, 2)
               << "dB,itd=" << juce::String(m.itdUs, 0) << "us]";
    result << " | maxAbsIld=" << juce::String(maxAbsIld, 2) << "dB"
           << " itd90=" << juce::String(itd90Mag, 0) << "us"
           << " ild0=" << juce::String(ild0, 2) << "dB"
           << " peak=" << juce::String(maxPeak, 4) << " finite=" << (finite ? "yes" : "no") << " failures=" << failures
           << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runSpatialRoutingNoiseImageTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr float fs = 48000.0f;
    constexpr int blockSize = 512;
    constexpr int warmupBlocks = 100;
    constexpr int captureBlocks = 180;

    struct NoiseMetrics
    {
        float ildDb = 0.0f;
        float rmsDb = -100.0f;
        float totalRms = 0.0f;
        float corr = 1.0f;
        float peak = 0.0f;
        int invalid = 0;
    };

    auto renderCase = [&](int spatialMode, float azimuth) -> NoiseMetrics
    {
        EchoesModel model;
        model.Reset(fs, 2, blockSize);
        model.getSpatialField()->getVoiceProcessor(0)->useDefaultHRTF();
        model.getSpatialField()->getVoiceProcessor(0)->setDebugDisableEarlyReflections(true);
        model.getSpatialField()->getVoiceProcessor(0)->setDebugDisableAirAbsorption(true);
        model.getSpatialField()->getVoiceProcessor(0)->setDebugDisableWidth(true);
        model.getSpatialField()->getVoiceProcessor(0)->setDebugDisableDiffuseEQ(true);

        auto updateModel = [&]()
        {
            int heads[3] = {1, 1, 1};
            model.UpdateParameters(0.5f, 0.5f, 0.0f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, true, 1.0f, 0.65f, 0.0f, 0.0f,
                                   0.0f, 0, false, 0.0f, 0.0f, 0.0f, true, azimuth, 0.0f, 1.0f, 100.0f, 20.0f, 2, 0.5f,
                                   0.5f, 0.5f, 0, 0, -1.0f, -1.0f, 1.0f, 1.0f, true, spatialMode, 1.0f, 0.0f);
        };

        std::vector<std::vector<float>> buffer(2, std::vector<float>(static_cast<size_t>(blockSize), 0.0f));

        double sumL = 0.0;
        double sumR = 0.0;
        double sumLL = 0.0;
        double sumRR = 0.0;
        double sumLR = 0.0;
        int64_t sampleCount = 0;
        NoiseMetrics metrics;

        for (int b = 0; b < warmupBlocks + captureBlocks; ++b)
        {
            updateModel();
            for (auto& channel : buffer)
                std::fill(channel.begin(), channel.end(), 0.0f);

            model.ProcessBuffer(buffer, blockSize);

            if (b < warmupBlocks)
                continue;

            for (int s = 0; s < blockSize; ++s)
            {
                const float l = buffer[0][static_cast<size_t>(s)];
                const float r = buffer[1][static_cast<size_t>(s)];
                if (!std::isfinite(l) || !std::isfinite(r))
                {
                    ++metrics.invalid;
                    continue;
                }

                metrics.peak = juce::jmax(metrics.peak, juce::jmax(std::abs(l), std::abs(r)));
                sumL += l;
                sumR += r;
                sumLL += static_cast<double>(l) * static_cast<double>(l);
                sumRR += static_cast<double>(r) * static_cast<double>(r);
                sumLR += static_cast<double>(l) * static_cast<double>(r);
                ++sampleCount;
            }
        }

        const double invN = (sampleCount > 0) ? 1.0 / static_cast<double>(sampleCount) : 0.0;
        const double meanL = sumL * invN;
        const double meanR = sumR * invN;
        const double varL = std::max(0.0, sumLL * invN - meanL * meanL);
        const double varR = std::max(0.0, sumRR * invN - meanR * meanR);
        const double cov = sumLR * invN - meanL * meanR;
        const float rmsL = static_cast<float>(std::sqrt(varL));
        const float rmsR = static_cast<float>(std::sqrt(varR));

        metrics.ildDb = TestUtilities::linearToDb(rmsR / juce::jmax(rmsL, 1.0e-12f));
        metrics.totalRms = static_cast<float>(std::sqrt(0.5 * (varL + varR)));
        metrics.rmsDb = TestUtilities::linearToDb(metrics.totalRms);
        metrics.corr = (varL > 1.0e-24 && varR > 1.0e-24) ? static_cast<float>(cov / std::sqrt(varL * varR)) : 1.0f;
        return metrics;
    };

    const NoiseMetrics postPos = renderCase(0, 90.0f);
    const NoiseMetrics postNeg = renderCase(0, -90.0f);
    const NoiseMetrics prePos = renderCase(1, 90.0f);
    const NoiseMetrics preNeg = renderCase(1, -90.0f);

    const std::array<NoiseMetrics, 4> cases{postPos, postNeg, prePos, preNeg};

    int invalid = 0;
    float maxPeak = 0.0f;
    float minRms = std::numeric_limits<float>::max();
    float maxAbsIld = 0.0f;
    float maxAbsCorr = 0.0f;
    for (const auto& metrics : cases)
    {
        invalid += metrics.invalid;
        maxPeak = juce::jmax(maxPeak, metrics.peak);
        minRms = juce::jmin(minRms, metrics.totalRms);
        maxAbsIld = juce::jmax(maxAbsIld, std::abs(metrics.ildDb));
        maxAbsCorr = juce::jmax(maxAbsCorr, std::abs(metrics.corr));
    }

    const float postAzimuthDelta = std::abs(postPos.ildDb - postNeg.ildDb);
    const float preAzimuthDelta = std::abs(prePos.ildDb - preNeg.ildDb);
    const float routePosDelta = std::abs(postPos.ildDb - prePos.ildDb);
    const float routeNegDelta = std::abs(postNeg.ildDb - preNeg.ildDb);
    const float routeRmsPosDelta = std::abs(postPos.rmsDb - prePos.rmsDb);
    const float routeRmsNegDelta = std::abs(postNeg.rmsDb - preNeg.rmsDb);
    const float maxAzimuthDelta = juce::jmax(postAzimuthDelta, preAzimuthDelta);
    const float maxRouteIldDelta = juce::jmax(routePosDelta, routeNegDelta);
    const float maxRouteRmsDelta = juce::jmax(routeRmsPosDelta, routeRmsNegDelta);

    int failures = 0;
    if (invalid != 0)
        ++failures;
    if (minRms < TestUtilities::dbToLinear(-98.0f))
        ++failures;
    if (maxPeak > 0.05f)
        ++failures;
    if (maxAbsCorr > 0.35f)
        ++failures;
    if (maxAbsIld > 3.0f)
        ++failures;
    if (maxAzimuthDelta > 1.0f)
        ++failures;
    if (maxRouteIldDelta > 1.0f || maxRouteRmsDelta > 1.5f)
        ++failures;

    const bool passed = failures == 0;
    juce::String result;
    result << "SpatialRoutingNoiseImage:"
           << " post(+/-)=" << juce::String(postPos.ildDb, 2) << "/" << juce::String(postNeg.ildDb, 2) << "dB"
           << " pre(+/-)=" << juce::String(prePos.ildDb, 2) << "/" << juce::String(preNeg.ildDb, 2) << "dB"
           << " routeIldDelta=" << juce::String(maxRouteIldDelta, 2) << "dB"
           << " azDelta=" << juce::String(maxAzimuthDelta, 2) << "dB"
           << " routeRmsDelta=" << juce::String(maxRouteRmsDelta, 2) << "dB"
           << " corrMax=" << juce::String(maxAbsCorr, 3)
           << " rmsMin=" << juce::String(TestUtilities::linearToDb(minRms), 1) << "dB"
           << " peak=" << juce::String(maxPeak, 4) << " invalid=" << invalid << " failures=" << failures << " "
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runSpatialRoutingModeSwitchStabilityTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // Given: EchoesModel running with binaural enabled and delay active.
    // When:  SpatialRoutingMode is toggled every ~50ms during continuous audio processing.
    // Then:  No NaN/Inf in output, peak < +6 dBFS, no excessive inter-sample clicks.

    const float fs = 48000.0f;
    const int blockSize = 256;
    const float durationSec = 2.0f;
    const int totalBlocks = static_cast<int>((fs * durationSec) / blockSize);
    const int switchIntervalBlocks = std::max(1, static_cast<int>(0.05f * fs / blockSize));

    EchoesModel model;
    model.Reset(fs, 2, blockSize);
    model.getSpatialField()->getVoiceProcessor(0)->useDefaultHRTF();

    std::vector<std::vector<float>> buf(2, std::vector<float>(blockSize, 0.0f));

    const float phaseInc = 2.0f * juce::MathConstants<float>::pi * 440.0f / fs;
    float phase = 0.0f;
    int currentMode = 0;

    bool hasNanInf = false;
    float peakSample = 0.0f;
    float maxDelta = 0.0f;
    float prevSample = 0.0f;
    bool hasPrev = false;

    for (int b = 0; b < totalBlocks; ++b)
    {
        if (b % switchIntervalBlocks == 0)
            currentMode = 1 - currentMode;

        int heads[3] = {1, 1, 0};
        model.UpdateParameters(0.5f, 0.5f, 0.5f, 0.5f, heads, 1, 0.7f, 1, 0.5f, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.5f,
                               0.5f, 0, false, 0.0f, 0.0f, 0.0f, true, 90.0f, 0.0f, 1.0f, 100.0f, 150.0f, 2, 0.5f, 0.5f,
                               0.5f, 0, 0, -1.0f, -1.0f, 1.0f, 1.0f, true, currentMode, 1.0f, 1.0f, 0.0f,
                               // size .5, decay .5, tone from damping .3, mod .08, character=springBalance .5
                               ReverbMacroParams{0.5f, 0.5f, 0.73f, 0.08f, 0.5f, 0.0f, 0.0f, 0});

        for (int i = 0; i < blockSize; ++i)
        {
            float sample = 0.4f * std::sin(phase);
            phase += phaseInc;
            if (phase >= 2.0f * juce::MathConstants<float>::pi)
                phase -= 2.0f * juce::MathConstants<float>::pi;
            buf[0][static_cast<size_t>(i)] = sample;
            buf[1][static_cast<size_t>(i)] = sample;
        }

        model.ProcessBuffer(buf, blockSize);

        for (int ch = 0; ch < 2; ++ch)
        {
            for (int i = 0; i < blockSize; ++i)
            {
                float s = buf[static_cast<size_t>(ch)][static_cast<size_t>(i)];
                if (std::isnan(s) || std::isinf(s))
                    hasNanInf = true;
                peakSample = std::max(peakSample, std::abs(s));

                if (hasPrev && ch == 0)
                {
                    float delta = std::abs(s - prevSample);
                    maxDelta = std::max(maxDelta, delta);
                }
                if (ch == 0)
                {
                    prevSample = s;
                    hasPrev = true;
                }
            }
        }
    }

    const bool bounded = peakSample < 2.0f;
    const bool noNanInf = !hasNanInf;
    const bool noClicks = maxDelta < 0.8f;
    const bool passed = noNanInf && bounded && noClicks;

    juce::String result;
    result << "SpatialRoutingModeSwitch: peak=" << juce::String(peakSample, 4)
           << " maxDelta=" << juce::String(maxDelta, 4) << " nanInf=" << (hasNanInf ? "YES" : "no")
           << " switches=" << (totalBlocks / switchIntervalBlocks) << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runSpatialRoutingPresetRoundTripTest(SpatialEchoAudioProcessor& processor)
{
    // Given: SpatialRouting parameter set to "Before Effects" (index 1).
    // When:  State is saved and loaded into a fresh processor.
    // Then:  SpatialRouting reads back as index 1.
    //        Also: a state blob without SpatialRouting loads it as 0.

    auto& vts = processor.getValueTreeState();

    auto* routingParam = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter("SpatialRouting"));
    if (routingParam == nullptr)
        return "SpatialRoutingPresetRoundTrip: SpatialRouting param not found [FAIL]";

    // Set to "Before Effects" (index 1)
    {
        const int maxIdx = std::max(0, routingParam->choices.size() - 1);
        const float norm = (maxIdx > 0) ? 1.0f / static_cast<float>(maxIdx) : 0.0f;
        routingParam->setValueNotifyingHost(norm);
    }

    const int beforeSave = routingParam->getIndex();
    if (beforeSave != 1)
        return "SpatialRoutingPresetRoundTrip: Failed to set mode=1 (got " + juce::String(beforeSave) + ") [FAIL]";

    // Save state
    juce::MemoryBlock stateBlock;
    processor.getStateInformation(stateBlock);

    // Load into fresh processor
    SpatialEchoAudioProcessor probe;
    probe.prepareToPlay(48000.0, 512);
    probe.setStateInformation(stateBlock.getData(), static_cast<int>(stateBlock.getSize()));

    auto* probeRouting =
        dynamic_cast<juce::AudioParameterChoice*>(probe.getValueTreeState().getParameter("SpatialRouting"));
    if (probeRouting == nullptr)
        return "SpatialRoutingPresetRoundTrip: param not found in probe [FAIL]";

    const int afterLoad = probeRouting->getIndex();
    const bool roundTripOk = (afterLoad == 1);

    // A state without SpatialRouting: strip it from the XML, reload, expect 0.
    auto xml = juce::AudioProcessor::getXmlFromBinary(stateBlock.getData(), static_cast<int>(stateBlock.getSize()));
    bool legacyOk = false;
    if (xml != nullptr)
    {
        std::function<bool(juce::XmlElement*, const juce::String&)> removeParam =
            [&](juce::XmlElement* root, const juce::String& paramId) -> bool
        {
            if (root == nullptr)
                return false;
            for (auto* child : root->getChildIterator())
            {
                if (child->hasTagName("PARAM") && child->getStringAttribute("id") == paramId)
                {
                    root->removeChildElement(child, true);
                    return true;
                }
                if (removeParam(child, paramId))
                    return true;
            }
            return false;
        };
        removeParam(xml.get(), "SpatialRouting");

        juce::MemoryBlock legacyBlock;
        juce::AudioProcessor::copyXmlToBinary(*xml, legacyBlock);

        SpatialEchoAudioProcessor legacyProbe;
        legacyProbe.prepareToPlay(48000.0, 512);
        legacyProbe.setStateInformation(legacyBlock.getData(), static_cast<int>(legacyBlock.getSize()));

        auto* legacyRouting =
            dynamic_cast<juce::AudioParameterChoice*>(legacyProbe.getValueTreeState().getParameter("SpatialRouting"));
        legacyOk = (legacyRouting != nullptr && legacyRouting->getIndex() == 0);
    }

    // Restore original processor to default
    routingParam->setValueNotifyingHost(0.0f);

    const bool passed = roundTripOk && legacyOk;

    juce::String result;
    result << "SpatialRoutingPresetRoundTrip: saved=1 loaded=" << afterLoad
           << " roundTrip=" << (roundTripOk ? "ok" : "FAIL") << " legacyDefault=" << (legacyOk ? "ok" : "FAIL") << " "
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runSpatialRoutingMonoSafetyTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // Given: EchoesModel with 1 channel (mono), SpatialRoutingMode=1 (Before Effects),
    //        binaural enabled.
    // When:  Audio is processed.
    // Then:  No crash, no NaN/Inf, output bounded.

    const float fs = 48000.0f;
    const int blockSize = 256;
    const int numBlocks = 100;

    EchoesModel model;
    model.Reset(fs, 1, blockSize);
    model.getSpatialField()->getVoiceProcessor(0)->useDefaultHRTF();

    int heads[3] = {1, 0, 0};
    model.UpdateParameters(0.5f, 0.5f, 0.5f, 0.5f, heads, 1, 0.7f, 0, 0.5f, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.5f, 0.5f,
                           0, false, 0.0f, 0.0f, 0.0f, true, 45.0f, 15.0f, 1.0f, 100.0f, 150.0f, 2, 0.5f, 0.5f, 0.5f, 0,
                           0, -1.0f, -1.0f, 1.0f, 1.0f, true, 1);

    std::vector<std::vector<float>> buf(1, std::vector<float>(blockSize, 0.0f));

    const float phaseInc = 2.0f * juce::MathConstants<float>::pi * 440.0f / fs;
    float phase = 0.0f;
    bool hasNanInf = false;
    float peakSample = 0.0f;

    for (int b = 0; b < numBlocks; ++b)
    {
        for (int i = 0; i < blockSize; ++i)
        {
            buf[0][static_cast<size_t>(i)] = 0.4f * std::sin(phase);
            phase += phaseInc;
            if (phase >= 2.0f * juce::MathConstants<float>::pi)
                phase -= 2.0f * juce::MathConstants<float>::pi;
        }

        model.ProcessBuffer(buf, blockSize);

        for (int i = 0; i < blockSize; ++i)
        {
            float s = buf[0][static_cast<size_t>(i)];
            if (std::isnan(s) || std::isinf(s))
                hasNanInf = true;
            peakSample = std::max(peakSample, std::abs(s));
        }
    }

    const bool noNanInf = !hasNanInf;
    const bool bounded = peakSample < 2.0f;
    const bool passed = noNanInf && bounded;

    juce::String result;
    result << "SpatialRoutingMonoSafety: peak=" << juce::String(peakSample, 4)
           << " nanInf=" << (hasNanInf ? "YES" : "no") << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

}  // namespace SpatialEchoTests::System

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
