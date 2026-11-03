// BinauralSelfTests: Self-tests that reach BinauralProcessor internals (convolution engine, SLERP, crossover)
//   through the BinauralProcessorTestAccess friend.

#include "BinauralSelfTests.h"

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "../../DSP/BinauralProcessor.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

//==============================================================================
// Direct-convolution reference: no latency compensation or discarded samples.
//==============================================================================
juce::String BinauralProcessorTestAccess::runConvolutionParitySelfTest()
{
    using ConvolutionEngine = BinauralProcessor::ConvolutionEngine;
    constexpr int total = 4096;
    constexpr double gateDb = -90.0;
    const std::array<int, 8> irLengths{1, 63, 64, 65, 128, 200, 256, 512};
    const std::array<int, 6> preparedSizes{1, 7, 32, 64, 127, 512};
    const std::vector<std::vector<int>> schedules{
        {1}, {3}, {31}, {63}, {64}, {65}, {96}, {127}, {128}, {512}, {64, 1, 128, 17, 3, 511, 65}};
    juce::Random rng(0x5E17);
    std::vector<float> input(total);
    for (auto& sample : input)
        sample = rng.nextFloat() * 2.0f - 1.0f;

    auto direct = [&](const std::vector<float>& ir)
    {
        std::vector<double> result(total, 0.0);
        for (int n = 0; n < total; ++n)
            for (int tap = 0; tap <= n && tap < static_cast<int>(ir.size()); ++tap)
                result[static_cast<size_t>(n)] +=
                    static_cast<double>(ir[static_cast<size_t>(tap)]) * input[static_cast<size_t>(n - tap)];
        return result;
    };
    auto residual = [&](const std::vector<float>& actual, const std::vector<double>& expected)
    {
        double error = 0.0, energy = 0.0;
        for (int n = 0; n < total; ++n)
        {
            const double delta = actual[static_cast<size_t>(n)] - expected[static_cast<size_t>(n)];
            if (!std::isfinite(delta))
                return 0.0;
            error += delta * delta;
            energy += expected[static_cast<size_t>(n)] * expected[static_cast<size_t>(n)];
        }
        return 10.0 * std::log10(juce::jmax(1.0e-30, error / juce::jmax(energy, 1.0e-30)));
    };

    double worstStaticDb = -300.0, worstTransitionDb = -300.0, worstResetDb = -300.0;
    bool zeroLatency = true;
    int cases = 0;
    for (int length : irLengths)
    {
        std::vector<float> ir(static_cast<size_t>(length));
        for (auto& tap : ir)
            tap = rng.nextFloat() * 2.0f - 1.0f;
        // The last prepared tap must survive, especially at partition edges.
        ir.back() = 0.75f;
        const auto reference = direct(ir);
        for (int prepared : preparedSizes)
        {
            for (const auto& schedule : schedules)
            {
                ConvolutionEngine engine;
                engine.prepare(48000.0, prepared, length);
                engine.setImpulseResponse(ir.data(), length, true);
                std::vector<float> actual(total, 0.0f);
                int pos = 0;
                size_t block = 0;
                while (pos < total)
                {
                    const int count = juce::jmin(schedule[block++ % schedule.size()], total - pos);
                    engine.process(input.data() + pos, actual.data() + pos, count);
                    zeroLatency = zeroLatency && engine.getLatencySamples() == 0;
                    pos += count;
                }
                worstStaticDb = juce::jmax(worstStaticDb, residual(actual, reference));
                ++cases;
            }
        }
    }

    std::vector<float> irA(512), irB(200), irC(65);
    for (auto* ir : {&irA, &irB, &irC})
        for (auto& tap : *ir)
            tap = rng.nextFloat() * 2.0f - 1.0f;
    const auto refA = direct(irA), refB = direct(irB), refC = direct(irC);
    constexpr int firstChange = 173, secondChange = 194, thirdChange = 389, ignoredChange = 401;
    constexpr double fadeDenominator = ConvolutionEngine::kMaxPartitionSize - 1;
    std::vector<double> transitionReference(total);
    for (int n = 0; n < total; ++n)
    {
        const auto index = static_cast<size_t>(n);
        const double a = refA[index], b = refB[index], c = refC[index];
        if (n < firstChange)
            transitionReference[index] = a;
        else if (n < secondChange)
            transitionReference[index] = a + (b - a) * (n - firstChange) / fadeDenominator;
        else if (n < thirdChange)
        {
            const double frozen = a + (b - a) * (secondChange - firstChange) / fadeDenominator;
            const double t = juce::jmin(1.0, (n - secondChange) / fadeDenominator);
            transitionReference[index] = frozen + (c - frozen) * t;
        }
        else
        {
            const double t = juce::jmin(1.0, (n - thirdChange) / fadeDenominator);
            transitionReference[index] = c + (a - c) * t;
        }
    }
    for (const auto& schedule : schedules)
    {
        ConvolutionEngine engine;
        engine.prepare(48000.0, 512, 512);
        engine.setImpulseResponse(irA.data(), static_cast<int>(irA.size()), true);
        std::vector<float> actual(total, 0.0f);
        int pos = 0;
        size_t block = 0;
        while (pos < total)
        {
            if (pos == firstChange)
                engine.setImpulseResponse(irB.data(), static_cast<int>(irB.size()), true);
            if (pos == secondChange)
                engine.setImpulseResponse(irC.data(), static_cast<int>(irC.size()), true);
            if (pos == thirdChange)
                engine.setImpulseResponse(irA.data(), static_cast<int>(irA.size()), true);
            if (pos == ignoredChange)
                engine.setImpulseResponse(irB.data(), static_cast<int>(irB.size()), false);
            int count = juce::jmin(schedule[block++ % schedule.size()], total - pos);
            for (int event : {firstChange, secondChange, thirdChange, ignoredChange})
                if (event > pos)
                    count = juce::jmin(count, event - pos);
            engine.process(input.data() + pos, actual.data() + pos, count);
            zeroLatency = zeroLatency && engine.getLatencySamples() == 0;
            pos += count;
        }
        worstTransitionDb = juce::jmax(worstTransitionDb, residual(actual, transitionReference));

        // Reset in the middle of both a hop and a fade. It must clear both
        // histories, retain the selected IR, and support in-place processing.
        engine.process(input.data(), actual.data(), 17);
        engine.setImpulseResponse(irB.data(), static_cast<int>(irB.size()), true);
        engine.process(input.data(), actual.data(), 3);
        engine.reset();
        actual = input;
        pos = 0;
        block = 0;
        while (pos < total)
        {
            const int count = juce::jmin(schedule[block++ % schedule.size()], total - pos);
            engine.process(actual.data() + pos, actual.data() + pos, count);
            pos += count;
        }
        worstResetDb = juce::jmax(worstResetDb, residual(actual, refB));
        ++cases;
    }

    const bool passed = worstStaticDb < gateDb && worstTransitionDb < gateDb && worstResetDb < gateDb && zeroLatency;
    juce::String result;
    result << "BinauralConvolutionParity: cases=" << cases << " callbackErr=" << juce::String(worstStaticDb, 1) << "dB"
           << " transitionErr=" << juce::String(worstTransitionDb, 1) << "dB"
           << " resetInPlaceErr=" << juce::String(worstResetDb, 1) << "dB"
           << " gate=" << juce::String(gateDb, 0) << "dB expectedGain=1.0"
           << " latency=" << (zeroLatency ? "0" : "nonzero") << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

//==============================================================================
// Control continuity: bypass, wet-mix and enable ramps against an analytic
// mixture reference, and SLERP trajectories against exact great-circle arcs.
//==============================================================================
juce::String BinauralProcessorTestAccess::runControlContinuitySelfTest()
{
    using SphericalCoord = BinauralProcessor::SphericalCoord;
    constexpr double fs = 48000.0;
    constexpr int preparedBlock = 512;
    constexpr int rampSamples = 2400;
    constexpr int captureSamples = 5600;
    double maxMixError = 0.0;
    double wetDrySeparation = 0.0;
    bool finite = true;
    int mixtureCases = 0;

    for (const auto& callbacks : {std::vector<int>{64}, std::vector<int>{512}, std::vector<int>{17, 257, 3, 64, 511}})
    {
        // Disable, wet=0, interrupted disable, enable, and a small nonzero mix.
        for (int scenario = 0; scenario < 5; ++scenario)
        {
            BinauralProcessor candidate, wetReference;
            const bool startsDry = scenario == 3;
            for (auto* processor : {&candidate, &wetReference})
            {
                processor->prepare(fs, preparedBlock, 2);
                processor->useDefaultHRTF();
                if (!processor->isHrtfLoaded())
                    return "BinauralControlContinuity: HRTF not loaded [FAIL]";
                processor->setEnabled(processor == &candidate ? !startsDry : true);
                processor->setWetMix(100.0f);
                processor->setSourceWidth(0.0f);
                processor->setDebugDisableEarlyReflections(true);
                processor->setDebugDisableAirAbsorption(true);
                processor->setDebugDisableNearFieldDvfForTest(true);
                processor->setDebugDisableNearFieldSafetyForTest(true);
                processor->snapToPosition(57.0f, 13.0f, 3.25f);
                processor->reset();
            }

            if (scenario == 0 || scenario == 2)
                candidate.setEnabled(false);
            else if (scenario == 1)
                candidate.setWetMix(0.0f);
            else if (scenario == 3)
                candidate.setEnabled(true);
            else
                candidate.setWetMix(0.005f);

            juce::AudioBuffer<float> actual(2, preparedBlock), wet(2, preparedBlock);
            double segmentStartWeight = startsDry ? 0.0 : 1.0;
            double targetWeight = startsDry ? 1.0 : (scenario == 4 ? 0.00005 : 0.0);
            int segmentStart = 0;
            int processed = 0;
            size_t callback = 0;
            while (processed < captureSamples)
            {
                if (scenario == 2 && processed == 800)
                {
                    segmentStartWeight = 1.0 - 800.0 / rampSamples;
                    segmentStart = 800;
                    targetWeight = 1.0;
                    candidate.setEnabled(true);
                }
                int count = std::min(callbacks[callback++ % callbacks.size()], captureSamples - processed);
                if (scenario == 2 && processed < 800)
                    count = std::min(count, 800 - processed);
                actual.setSize(2, count, false, false, true);
                wet.setSize(2, count, false, false, true);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < count; ++i)
                    {
                        const double time = static_cast<double>(processed + i) / fs;
                        const float input = static_cast<float>(
                            0.17 * std::sin(2.0 * juce::MathConstants<double>::pi * 997.0 * time) +
                            (ch == 0 ? 0.11 : -0.09) * std::sin(2.0 * juce::MathConstants<double>::pi * 7313.0 * time));
                        actual.setSample(ch, i, input);
                        wet.setSample(ch, i, input);
                    }
                candidate.processBlock(actual);
                wetReference.processBlock(wet);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < count; ++i)
                    {
                        const double time = static_cast<double>(processed + i) / fs;
                        const float input = static_cast<float>(
                            0.17 * std::sin(2.0 * juce::MathConstants<double>::pi * 997.0 * time) +
                            (ch == 0 ? 0.11 : -0.09) * std::sin(2.0 * juce::MathConstants<double>::pi * 7313.0 * time));
                        const double fraction =
                            std::min(1.0, static_cast<double>(processed + i + 1 - segmentStart) / rampSamples);
                        const double weight = segmentStartWeight + (targetWeight - segmentStartWeight) * fraction;
                        const double expected = weight * wet.getSample(ch, i) + (1.0 - weight) * input;
                        const double observed = actual.getSample(ch, i);
                        finite = finite && std::isfinite(expected) && std::isfinite(observed) &&
                                 std::isfinite(wet.getSample(ch, i));
                        maxMixError = std::max(maxMixError, std::abs(observed - expected));
                        wetDrySeparation =
                            std::max(wetDrySeparation, std::abs(static_cast<double>(wet.getSample(ch, i)) - input));
                    }
                processed += count;
            }
            ++mixtureCases;
        }
    }

    // Double-precision angular distance supplies a reference independent of
    // the interpolation's tangent construction, including arbitrary antipodes.
    auto angularDistance = [](SphericalCoord a, SphericalCoord b)
    {
        const double radians = juce::MathConstants<double>::pi / 180.0;
        const double azA = a.azimuth * radians, elA = a.elevation * radians;
        const double azB = b.azimuth * radians, elB = b.elevation * radians;
        const double dot = std::sin(elA) * std::sin(elB) + std::cos(elA) * std::cos(elB) * std::cos(azA - azB);
        return std::acos(juce::jlimit(-1.0, 1.0, dot)) / radians;
    };
    const std::array<std::array<SphericalCoord, 2>, 8> paths{{{{{0.0f, 0.0f}, {180.0f, 0.0f}}},
                                                              {{{0.0f, 0.0f}, {-180.0f, 0.0f}}},
                                                              {{{0.0f, 0.0f}, {179.999f, 0.0f}}},
                                                              {{{0.0f, 0.0f}, {-179.999f, 0.0f}}},
                                                              {{{37.0f, 22.0f}, {-143.0f, -22.0f}}},
                                                              {{{0.0f, 90.0f}, {0.0f, -90.0f}}},
                                                              {{{-61.0f, -17.0f}, {94.0f, 35.0f}}},
                                                              {{{179.8f, 0.0f}, {-179.8f, 0.0f}}}}};
    double maxProgressError = 0.0;
    double maxArcError = 0.0;
    bool monotonic = true;
    bool normalized = true;
    for (const auto& path : paths)
    {
        const double arc = angularDistance(path[0], path[1]);
        double previousProgress = 0.0;
        for (int step = 0; step <= 1000; ++step)
        {
            const float t = static_cast<float>(step) / 1000.0f;
            const auto actual = BinauralProcessor::slerpPosition(path[0], path[1], t);
            const double progress = angularDistance(path[0], actual);
            const double remaining = angularDistance(actual, path[1]);
            finite = finite && std::isfinite(actual.azimuth) && std::isfinite(actual.elevation) &&
                     std::isfinite(progress) && std::isfinite(remaining);
            normalized = normalized && std::abs(actual.azimuth) <= 180.0f && std::abs(actual.elevation) <= 90.0f;
            monotonic = monotonic && progress + 0.0001 >= previousProgress;
            maxProgressError = std::max(maxProgressError, std::abs(progress - static_cast<double>(t) * arc));
            maxArcError = std::max(maxArcError, std::abs(progress + remaining - arc));
            previousProgress = progress;
        }
    }
    // Unsigned progress cannot distinguish the two antipodal half-circles.
    // Assert the declared signed-azimuth and pole-meridian choices directly.
    const std::array<std::array<SphericalCoord, 3>, 3> branchChecks{
        {{{{0.0f, 0.0f}, {180.0f, 0.0f}, {90.0f, 0.0f}}},
         {{{0.0f, 0.0f}, {-180.0f, 0.0f}, {-90.0f, 0.0f}}},
         {{{37.0f, 90.0f}, {37.0f, -90.0f}, {37.0f, 0.0f}}}}};
    double maxBranchError = 0.0;
    for (const auto& check : branchChecks)
    {
        const auto midpoint = BinauralProcessor::slerpPosition(check[0], check[1], 0.5f);
        const double error = angularDistance(midpoint, check[2]);
        finite = finite && std::isfinite(midpoint.azimuth) && std::isfinite(midpoint.elevation) && std::isfinite(error);
        maxBranchError = std::max(maxBranchError, error);
    }
    const bool passed = finite && normalized && monotonic && mixtureCases == 15 && wetDrySeparation > 0.05 &&
                        maxMixError < 2.0e-5 && maxProgressError < 1.0e-4 && maxArcError < 1.0e-4 &&
                        maxBranchError < 1.0e-4;
    juce::String result;
    result << "BinauralControlContinuity: mixtureCases=" << mixtureCases
           << " maxMixError=" << juce::String(maxMixError, 8)
           << " wetDrySeparation=" << juce::String(wetDrySeparation, 4)
           << " trajectorySamples=" << static_cast<int>(paths.size()) * 1001
           << " maxProgressErrorDeg=" << juce::String(maxProgressError, 7)
           << " maxArcErrorDeg=" << juce::String(maxArcError, 7)
           << " maxBranchErrorDeg=" << juce::String(maxBranchError, 7) << " monotonic=" << (monotonic ? "yes" : "no")
           << " finite=" << (finite ? "yes" : "no") << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

//==============================================================================
// Per-path latency / crossover-integrity measurement.
//
// The binaural stage splits at kLowMidCrossover: the low band is a delay-line
// path (crossover LP + ITD + alignment offset) and the high band is a
// convolution path (crossover HP + HRIR + partitioned convolution). Two
// separate things have to hold, and they need different measurements:
//
//   * DELAY vs the host. Read as group delay off the captured impulse
//     response. The dry blend must be at zero; callback-size changes must
//     leave every output sample unchanged, with no alignment compensation.
//
//   * CROSSOVER INTEGRITY. Comparing the low band's group delay to the high
//     band's is NOT a test of this: a Linkwitz-Riley pair is deliberately
//     dispersive (the LR4 lowpass alone carries ~145 samples of group delay at
//     60 Hz @48 kHz), and the pair still sums flat because the two halves are
//     phase-matched at the crossover by construction. What actually breaks if
//     one band is delayed relative to the other is the SUM, so this measures
//     the magnitude response through the crossover region and looks for the
//     notch that a relative delay produces.
//==============================================================================
juce::String BinauralProcessorTestAccess::runBinauralLatencySelfTest()
{
    constexpr double kSampleRate = 48000.0;
    constexpr int kPreparedBlock = 512;
    constexpr int kCaptureLength = 16384;
    bool capturedSamplesFinite = true;

    // Settle controls for the same sample count, then compare the complete
    // impulse response without shifting it or dropping callback transients.
    auto capture = [&](const std::vector<int>& callbackSizes, float wetPercent, bool antiPhase, std::vector<float>& out,
                       int& latencyReported) -> bool
    {
        BinauralProcessor bp;
        bp.prepare(kSampleRate, kPreparedBlock, 2);
        bp.useDefaultHRTF();
        if (!bp.isHrtfLoaded())
            return false;
        bp.setEnabled(true);
        bp.setWetMix(wetPercent);
        bp.setSourceWidth(0.0f);
        bp.setCrossoverFrequency(BinauralProcessor::kLowMidCrossover);
        bp.setDebugDisableEarlyReflections(true);
        bp.setDebugDisableAirAbsorption(true);
        bp.setDebugDisableNearFieldDvfForTest(true);
        bp.setDebugDisableNearFieldSafetyForTest(true);
        bp.snapToPosition(0.0f, 0.0f, 1.0f);
        bp.reset();

        juce::AudioBuffer<float> buffer(2, kPreparedBlock);
        int settled = 0;
        size_t callback = 0;
        while (settled < 8192)
        {
            const int count = juce::jmin(callbackSizes[callback++ % callbackSizes.size()], 8192 - settled);
            buffer.setSize(2, count, false, false, true);
            buffer.clear();
            bp.processBlock(buffer);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < count; ++i)
                    if (!std::isfinite(buffer.getSample(ch, i)))
                        capturedSamplesFinite = false;
            settled += count;
        }

        latencyReported = bp.leftEarConvA ? bp.leftEarConvA->getLatencySamples() : 0;
        out.assign(static_cast<size_t>(2 * kCaptureLength), 0.0f);
        int written = 0;
        callback = 0;
        while (written < kCaptureLength)
        {
            const int count = juce::jmin(callbackSizes[callback++ % callbackSizes.size()], kCaptureLength - written);
            buffer.setSize(2, count, false, false, true);
            buffer.clear();
            if (written == 0)
            {
                buffer.setSample(0, 0, 1.0f);
                buffer.setSample(1, 0, antiPhase ? -1.0f : 1.0f);
            }
            bp.processBlock(buffer);
            for (int ch = 0; ch < 2; ++ch)
            {
                for (int i = 0; i < count; ++i)
                {
                    const float sample = buffer.getSample(ch, i);
                    if (!std::isfinite(sample))
                        capturedSamplesFinite = false;
                    out[static_cast<size_t>(ch * kCaptureLength + written + i)] = sample;
                }
            }
            written += count;
            latencyReported = juce::jmax(latencyReported, bp.leftEarConvA->getLatencySamples());
        }
        return true;
    };

    auto dftAt = [&](const std::vector<float>& ir, double f, double& re, double& im)
    {
        re = 0.0;
        im = 0.0;
        for (size_t n = 0; n < static_cast<size_t>(kCaptureLength); ++n)
        {
            const double a = -2.0 * juce::MathConstants<double>::pi * f * static_cast<double>(n) / kSampleRate;
            re += ir[n] * std::cos(a);
            im += ir[n] * std::sin(a);
        }
    };

    auto magDb = [&](const std::vector<float>& ir, double f)
    {
        double re = 0.0, im = 0.0;
        dftAt(ir, f, re, im);
        return 20.0 * std::log10(std::sqrt(re * re + im * im) + 1.0e-12);
    };

    // Group delay from the phase slope across +-20 Hz. That span keeps the
    // phase step inside +-pi for any delay below ~12 ms, so no unwrapping.
    auto groupDelaySamples = [&](const std::vector<float>& ir, double freq)
    {
        const double delta = 20.0;
        auto phaseAt = [&](double f)
        {
            double re = 0.0, im = 0.0;
            dftAt(ir, f, re, im);
            return std::atan2(im, re);
        };
        double dPhi = phaseAt(freq + delta) - phaseAt(freq - delta);
        while (dPhi > juce::MathConstants<double>::pi)
            dPhi -= 2.0 * juce::MathConstants<double>::pi;
        while (dPhi < -juce::MathConstants<double>::pi)
            dPhi += 2.0 * juce::MathConstants<double>::pi;
        return -dPhi / (2.0 * juce::MathConstants<double>::pi * 2.0 * delta) * kSampleRate;
    };

    const std::vector<int> alternating{512, 1, 3, 127, 64, 17, 65, 96};
    std::vector<float> alignedIr, raggedIr, singleIr, alternatingIr, dryIr, mixedIr;
    std::vector<float> sideAlignedIr, sideAlternatingIr;
    int alignedLatency = -1, raggedLatency = -1, otherLatency = -1;
    bool latencyReportSane = true;
    if (!capture({512}, 100.0f, false, alignedIr, alignedLatency) ||
        !capture({100}, 100.0f, false, raggedIr, raggedLatency))
        return "BinauralLatency: HRTF not loaded [FAIL]";
    for (auto request : {0, 1, 2, 3, 4, 5})
    {
        bool ok = false;
        switch (request)
        {
            case 0:
                ok = capture({1}, 100.0f, false, singleIr, otherLatency);
                break;
            case 1:
                ok = capture(alternating, 100.0f, false, alternatingIr, otherLatency);
                break;
            case 2:
                ok = capture(alternating, 0.0f, false, dryIr, otherLatency);
                break;
            case 3:
                ok = capture(alternating, 50.0f, false, mixedIr, otherLatency);
                break;
            case 4:
                ok = capture({512}, 50.0f, true, sideAlignedIr, otherLatency);
                break;
            case 5:
                ok = capture(alternating, 50.0f, true, sideAlternatingIr, otherLatency);
                break;
        }
        if (!ok)
            return "BinauralLatency: HRTF not loaded [FAIL]";
        latencyReportSane = latencyReportSane && otherLatency == 0;
    }
    double callbackError = 0.0, wetDryError = 0.0;
    bool comparedSamplesFinite = true;
    auto accumulateError = [&](double actual, double expected, double& maxError)
    {
        const double delta = actual - expected;
        // jmax can discard NaN operands. Reject them explicitly for both ears,
        // including side-only captures that the left-ear DFT does not inspect.
        if (!std::isfinite(actual) || !std::isfinite(expected) || !std::isfinite(delta))
        {
            comparedSamplesFinite = false;
            return;
        }
        maxError = juce::jmax(maxError, std::abs(delta));
    };
    for (size_t n = 0; n < alignedIr.size(); ++n)
    {
        for (const auto* actual : {&raggedIr, &singleIr, &alternatingIr})
            accumulateError((*actual)[n], alignedIr[n], callbackError);
        accumulateError(sideAlignedIr[n], sideAlternatingIr[n], callbackError);
        accumulateError(mixedIr[n], 0.5 * (static_cast<double>(alignedIr[n]) + dryIr[n]), wetDryError);
    }

    const double dryDelay = groupDelaySamples(dryIr, 2000.0);
    const double alignedHigh = groupDelaySamples(alignedIr, 2000.0);
    const double raggedHigh = groupDelaySamples(raggedIr, 2000.0);
    const double alignedLow = groupDelaySamples(alignedIr, 60.0);

    // Crossover integrity: a relative delay between the two bands shows up as a
    // notch at the crossover. Compare the crossover frequency against the local
    // trend, taken as the mean of two points an equal factor either side of it
    // (60 Hz and 375 Hz around 150 Hz).
    const double fc = static_cast<double>(BinauralProcessor::kLowMidCrossover);
    const double belowDb = magDb(alignedIr, fc / 2.5);
    const double atDb = magDb(alignedIr, fc);
    const double aboveDb = magDb(alignedIr, fc * 2.5);
    const double crossoverDeviationDb = atDb - 0.5 * (belowDb + aboveDb);

    const double raggedBelowDb = magDb(raggedIr, fc / 2.5);
    const double raggedAtDb = magDb(raggedIr, fc);
    const double raggedAboveDb = magDb(raggedIr, fc * 2.5);
    const double raggedCrossoverDeviationDb = raggedAtDb - 0.5 * (raggedBelowDb + raggedAboveDb);

    // GATES.
    // 1. Dry must be sample-aligned with the input: at 0% wet the stage passes
    //    the input through, so any non-zero group delay means the dry blend
    //    picked up a delayed source.
    const bool dryIsZero = std::abs(dryDelay) < 1.0;
    // 2. No notch at the crossover, in EITHER alignment mode. +-1.5 dB is a
    //    real gate: a 63-sample relative delay between the bands (the ragged
    //    mode's worth) puts ~77 degrees of phase error at 150 Hz and digs a
    //    ~2 dB notch, so this fails if the low band stops tracking the high one.
    const bool crossoverIntact = std::abs(crossoverDeviationDb) < 1.5 && std::abs(raggedCrossoverDeviationDb) < 1.5;
    // 3. Every captured sample, including the first remainder boundary, must
    //    match the aligned reference; the 50% mix must null against wet+dry.
    const double raggedExtra = raggedHigh - alignedHigh;
    const bool callbacksMatch = callbackError < 2.0e-6 && std::abs(raggedExtra) < 0.01;
    const bool wetDryMatches = wetDryError < 2.0e-6;
    latencyReportSane = latencyReportSane && alignedLatency == 0 && raggedLatency == 0;
    const bool samplesFinite = capturedSamplesFinite && comparedSamplesFinite;
    const bool passed =
        samplesFinite && dryIsZero && crossoverIntact && callbacksMatch && wetDryMatches && latencyReportSane;

    juce::String result;
    result << "BinauralLatency: (samples @48k)"
           << " dry=" << juce::String(dryDelay, 1) << " wetHigh2k=" << juce::String(alignedHigh, 1)
           << " wetLow60=" << juce::String(alignedLow, 1) << " raggedHigh2k=" << juce::String(raggedHigh, 1)
           << " raggedExtra=" << juce::String(raggedExtra, 1) << " engineLatency=" << alignedLatency << "/"
           << raggedLatency << " | xover dB " << juce::String(belowDb, 2) << "/" << juce::String(atDb, 2) << "/"
           << juce::String(aboveDb, 2) << " dev=" << juce::String(crossoverDeviationDb, 2)
           << " raggedDev=" << juce::String(raggedCrossoverDeviationDb, 2)
           << " callbackMaxError=" << juce::String(callbackError, 9)
           << " wetDryMaxError=" << juce::String(wetDryError, 9) << " samplesFinite=" << (samplesFinite ? "yes" : "no")
           << " devGate=1.5dB callbackGate=0.000002 extraGate=0.01"
           << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

#endif
