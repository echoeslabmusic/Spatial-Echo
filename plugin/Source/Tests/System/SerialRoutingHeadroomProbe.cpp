// SerialRoutingHeadroomProbe: Serial-route reverb-input ownership and final-output headroom contract.

#include <vector>
#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "SystemTests.h"
#include "../../DSP/EchoesModel.h"
#include "EchoesModelTestAccess.h"
#include "../../DSP/TapeFlavors.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

namespace SpatialEchoTests::System
{
namespace
{
constexpr float kSampleRate = 48000.0f;
constexpr int kBlockSize = 256;
constexpr int kRenderSamples = static_cast<int>(6.0f * kSampleRate);
constexpr int kMeasureStart = static_cast<int>(4.0f * kSampleRate);
constexpr float kInputPeak = 0.2511886432f;                   // -12 dBFS peak
constexpr double kCanonicalReverbSendGainDb = -6.0205999133;  // 0.50x send pad
constexpr double kSendGainToleranceDb = 0.10;

double dbFromLinear(double value)
{
    return 20.0 * std::log10(std::max(1.0e-12, value));
}

struct RouteMetrics
{
    double reverbInputRms = 0.0;
    float reverbInputPeak = 0.0f;
    double reverbReturnRms = 0.0;
    float reverbReturnPeak = 0.0f;
    double echoPreTrimRms = 0.0;
    double finalOutputRms = 0.0;
    float finalOutputPeak = 0.0f;
    bool finite = true;
};

RouteMetrics renderRoute(int routingMode, int reverbEngine = 0)
{
    std::srand(static_cast<unsigned int>(0x51A7 + reverbEngine));
    EchoesModel model;
    model.Reset(kSampleRate, 2, kBlockSize);
    EchoesModelTestAccess::resetReverbInputProbe(model);
    EchoesModelTestAccess::enableReverbInputProbe(model, true);
    EchoesModelTestAccess::resetReverbReturnProbe(model);
    EchoesModelTestAccess::enableReverbReturnProbe(model, true);
    EchoesModelTestAccess::resetEchoPathPreTrimProbe(model);
    EchoesModelTestAccess::enableEchoPathPreTrimProbe(model, true);

    const auto warm = TapeFlavors::resolve(static_cast<int>(TapeFlavor::Warm), TapeFlavors::kDriveAnchor);
    int heads[3] = {1, 0, 0};
    ReverbMacroParams reverbMacros;
    reverbMacros.engine = reverbEngine;
    reverbMacros.mod = 0.0f;
    model.UpdateParameters(
        /*low*/ 0.5f, /*high*/ 0.5f,
        /*intensity*/ 0.35f, /*delaytime*/ 0.5f, heads, /*delayenabled*/ 1, /*mix*/ 1.0f,
        /*reverbenabled*/ 1, /*inputlevel gain*/ 1.0f, /*echoSend*/ 1.0f,
        /*noise*/ false, 0.0f, 0.0f,
        /*wow/flutter*/ 0.0f, 1.0f, 1.0f,
        /*stereoMode*/ 0,
        /*tempoSync*/ false, 0.0f, 0.0f, 0.0f,
        /*binaural*/ false, 0.0f, 0.0f, 1.0f, 100.0f, 150.0f, 2, warm.drive, warm.saturation, warm.bias,
        /*headRatioPreset*/ 0, routingMode,
        /*asperity*/ 0.0f, /*dropout*/ 0.0f,
        /*scrape*/ 1.0f, /*LPF scale*/ warm.lpfScale,
        /*feedbackCompensation*/ true,
        /*spatialRouting*/ 0,
        /*echoLevel*/ 1.0f, /*reverbLevel*/ 1.0f,
        /*duck*/ 0.0f, reverbMacros);

    std::vector<std::vector<float>> block(2, std::vector<float>(static_cast<size_t>(kBlockSize), 0.0f));
    double reverbInputEnergy = 0.0;
    int reverbInputCount = 0;
    double reverbReturnEnergy = 0.0;
    int reverbReturnCount = 0;
    double echoPreTrimEnergy = 0.0;
    int echoPreTrimCount = 0;
    double outputEnergy = 0.0;
    int outputCount = 0;
    RouteMetrics result;

    // Band-limited MULTITONE, not a single sine.
    //
    // This probe used one 997 Hz tone. That makes it a resonance lottery against a
    // reverb tank: sweeping Size alone was measured moving the Spring's apparent
    // sine gain from +0.3 dB to -21.5 dB, so a single-tone reading says more about
    // which mode the tone happens to excite than about the route's level. Eight
    // mutually incommensurate tones spanning 200 Hz - 4 kHz average over that,
    // which is the same in-band method used to establish the engines are unity.
    // Deterministic: fixed frequencies, fixed phases, peak-normalised once.
    static const float kToneHz[] = {211.0f, 337.0f, 523.0f, 811.0f, 1279.0f, 1993.0f, 3067.0f, 3907.0f};
    constexpr int kNumTones = 8;
    std::vector<float> stimulus(static_cast<size_t>(kRenderSamples), 0.0f);
    for (int n = 0; n < kNumTones; ++n)
    {
        const double inc = 2.0 * M_PI * static_cast<double>(kToneHz[n]) / kSampleRate;
        // Distinct starting phases keep the tones from all peaking together.
        double ph = 0.37 * static_cast<double>(n);
        for (int i = 0; i < kRenderSamples; ++i)
        {
            stimulus[static_cast<size_t>(i)] += static_cast<float>(std::sin(ph));
            ph += inc;
            if (ph >= 2.0 * M_PI)
                ph -= 2.0 * M_PI;
        }
    }
    float stimPeak = 0.0f;
    for (float v : stimulus)
        stimPeak = std::max(stimPeak, std::abs(v));
    const float stimScale = (stimPeak > 1.0e-9f) ? (kInputPeak / stimPeak) : 0.0f;
    for (float& v : stimulus)
        v *= stimScale;

    for (int blockStart = 0; blockStart < kRenderSamples; blockStart += kBlockSize)
    {
        for (int sample = 0; sample < kBlockSize; ++sample)
        {
            const int stimIndex = blockStart + sample;
            const float value = (stimIndex < kRenderSamples) ? stimulus[static_cast<size_t>(stimIndex)] : 0.0f;
            block[0][static_cast<size_t>(sample)] = value;
            block[1][static_cast<size_t>(sample)] = value;
        }

        model.ProcessBuffer(block, kBlockSize);
        if (blockStart < kMeasureStart)
            continue;

        float reverbPeak = 0.0f;
        float reverbRms = 0.0f;
        result.finite = result.finite &&
                        EchoesModelTestAccess::getReverbInputMetrics(model, routingMode, reverbPeak, reverbRms) &&
                        std::isfinite(reverbPeak) && std::isfinite(reverbRms);
        result.reverbInputPeak = std::max(result.reverbInputPeak, reverbPeak);
        reverbInputEnergy += static_cast<double>(reverbRms) * reverbRms * kBlockSize * 2;
        reverbInputCount += kBlockSize * 2;

        float reverbReturnPeak = 0.0f;
        float reverbReturnRms = 0.0f;
        result.finite =
            result.finite &&
            EchoesModelTestAccess::getReverbReturnMetrics(model, routingMode, reverbReturnPeak, reverbReturnRms) &&
            std::isfinite(reverbReturnPeak) && std::isfinite(reverbReturnRms);
        result.reverbReturnPeak = std::max(result.reverbReturnPeak, reverbReturnPeak);
        reverbReturnEnergy += static_cast<double>(reverbReturnRms) * reverbReturnRms * kBlockSize * 2;
        reverbReturnCount += kBlockSize * 2;

        float echoPreTrimPeak = 0.0f;
        float echoPreTrimRms = 0.0f;
        result.finite = result.finite &&
                        EchoesModelTestAccess::getEchoPathPreTrimMetrics(model, echoPreTrimPeak, echoPreTrimRms) &&
                        std::isfinite(echoPreTrimPeak) && std::isfinite(echoPreTrimRms);
        echoPreTrimEnergy += static_cast<double>(echoPreTrimRms) * echoPreTrimRms * kBlockSize * 2;
        echoPreTrimCount += kBlockSize * 2;

        for (int channel = 0; channel < 2; ++channel)
        {
            for (int sample = 0; sample < kBlockSize; ++sample)
            {
                const float value = block[static_cast<size_t>(channel)][static_cast<size_t>(sample)];
                result.finite = result.finite && std::isfinite(value);
                result.finalOutputPeak = std::max(result.finalOutputPeak, std::abs(value));
                outputEnergy += static_cast<double>(value) * static_cast<double>(value);
                ++outputCount;
            }
        }
    }

    result.reverbInputRms = reverbInputCount > 0 ? std::sqrt(reverbInputEnergy / reverbInputCount) : 0.0;
    result.reverbReturnRms = reverbReturnCount > 0 ? std::sqrt(reverbReturnEnergy / reverbReturnCount) : 0.0;
    result.echoPreTrimRms = echoPreTrimCount > 0 ? std::sqrt(echoPreTrimEnergy / echoPreTrimCount) : 0.0;
    result.finalOutputRms = outputCount > 0 ? std::sqrt(outputEnergy / outputCount) : 0.0;
    return result;
}
}  // namespace

juce::String runSerialRoutingHeadroomProbe(SpatialEchoAudioProcessor& processor)
{
    juce::ignoreUnused(processor);
    const auto echoToReverb = renderRoute(1);
    const auto reverbToEcho = renderRoute(2);
    const double inputRmsDeltaDb =
        dbFromLinear(echoToReverb.reverbInputRms) - dbFromLinear(reverbToEcho.reverbInputRms);
    const double echoToReverbSendGainDb =
        dbFromLinear(echoToReverb.reverbInputRms) - dbFromLinear(echoToReverb.echoPreTrimRms);
    const bool sendGainOwnedByReverbPath =
        std::abs(echoToReverbSendGainDb - kCanonicalReverbSendGainDb) <= kSendGainToleranceDb;
    const bool finite = echoToReverb.finite && reverbToEcho.finite;

    auto* root = new juce::DynamicObject();
    root->setProperty("test", "SerialRoutingHeadroomProbe");
    root->setProperty("schema", "spatial-echo.serial-routing-headroom-probe.v2");
    root->setProperty(
        "configuration",
        "Release; deterministic; 48kHz/256; sustained 997Hz sine at -12dBFS peak; H1; intensity=0.35; Warm anchor "
        "tape; 6s render; final 2s measured; output protection/noise/modulation/binaural off");

    auto* route1 = new juce::DynamicObject();
    route1->setProperty("routing", "Echo->Reverb");
    route1->setProperty("reverb_input_rms_linear", echoToReverb.reverbInputRms);
    route1->setProperty("reverb_input_rms_dbfs", dbFromLinear(echoToReverb.reverbInputRms));
    route1->setProperty("reverb_input_peak_linear", echoToReverb.reverbInputPeak);
    route1->setProperty("echo_pretrim_rms_dbfs", dbFromLinear(echoToReverb.echoPreTrimRms));
    route1->setProperty("reverb_send_gain_db", echoToReverbSendGainDb);
    route1->setProperty("final_output_rms_linear", echoToReverb.finalOutputRms);
    route1->setProperty("final_output_rms_dbfs", dbFromLinear(echoToReverb.finalOutputRms));
    route1->setProperty("final_output_peak_linear", echoToReverb.finalOutputPeak);
    root->setProperty("route_1", juce::var(route1));

    auto* route2 = new juce::DynamicObject();
    route2->setProperty("routing", "Reverb->Echo");
    route2->setProperty("reverb_input_rms_linear", reverbToEcho.reverbInputRms);
    route2->setProperty("reverb_input_rms_dbfs", dbFromLinear(reverbToEcho.reverbInputRms));
    route2->setProperty("reverb_input_peak_linear", reverbToEcho.reverbInputPeak);
    route2->setProperty("final_output_rms_linear", reverbToEcho.finalOutputRms);
    route2->setProperty("final_output_rms_dbfs", dbFromLinear(reverbToEcho.finalOutputRms));
    route2->setProperty("final_output_peak_linear", reverbToEcho.finalOutputPeak);
    root->setProperty("route_2", juce::var(route2));

    root->setProperty("reverb_input_rms_route1_minus_route2_db", inputRmsDeltaDb);
    root->setProperty("finite", finite);
    root->setProperty("echo_to_reverb_send_gain_owned_by_reverb_path", sendGainOwnedByReverbPath);

    const juce::File jsonFile = artifactDirectory().getChildFile("serial_routing_headroom_probe.json");
    jsonFile.deleteFile();
    const bool wroteJson = jsonFile.replaceWithText(juce::JSON::toString(juce::var(root), true));

    juce::String result;
    result << "SerialRoutingHeadroomProbe: Echo->Reverb inputRms="
           << juce::String(dbFromLinear(echoToReverb.reverbInputRms), 3)
           << "dBFS finalRms=" << juce::String(dbFromLinear(echoToReverb.finalOutputRms), 3)
           << "dBFS; Reverb->Echo inputRms=" << juce::String(dbFromLinear(reverbToEcho.reverbInputRms), 3)
           << "dBFS finalRms=" << juce::String(dbFromLinear(reverbToEcho.finalOutputRms), 3)
           << "dBFS; inputDelta=" << juce::String(inputRmsDeltaDb, 3) << "dB\n"
           << "Echo->Reverb sendGain=" << juce::String(echoToReverbSendGainDb, 3)
           << "dB target=" << juce::String(kCanonicalReverbSendGainDb, 3) << "+/-"
           << juce::String(kSendGainToleranceDb, 2) << "dB\n"
           << "[INFO] metrics: " << jsonFile.getFullPathName() << "\n"
           << ((finite && wroteJson && sendGainOwnedByReverbPath) ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runReverbRouteReturnParity(SpatialEchoAudioProcessor& processor)
{
    juce::ignoreUnused(processor);
    constexpr double kMaxRouteSpreadDb = 2.0;
    constexpr double kSignalFloor = 1.0e-6;
    constexpr std::array<const char*, 3> kRouteNames{{"Parallel", "Echo->Reverb", "Reverb->Echo"}};

    auto* root = new juce::DynamicObject();
    root->setProperty("test", "ReverbRouteReturnParity");
    root->setProperty("schema", "spatial-echo.reverb-route-return-parity.v1");
    root->setProperty("configuration", "48kHz/256; deterministic 997Hz -12dBFS peak; final 2s of 6s; "
                                       "normalized recovered-return gain = post-makeup return RMS / canonical "
                                       "post-pad reverb-input RMS; modulation/noise/spatial/protection off");
    root->setProperty("max_route_spread_db", kMaxRouteSpreadDb);

    bool finite = true;
    bool signal = true;
    bool canonicalSend = true;
    double worstRouteSpreadDb = 0.0;
    juce::Array<juce::var> engineRows;

    for (int engine = 0; engine <= 1; ++engine)
    {
        auto* engineObject = new juce::DynamicObject();
        engineObject->setProperty("engine", engine == 0 ? "Spring" : "Cloud");
        juce::Array<juce::var> routeRows;
        double minGainDb = 0.0;
        double maxGainDb = 0.0;
        bool any = false;

        for (int route = 0; route < 3; ++route)
        {
            const auto metrics = renderRoute(route, engine);
            const double returnGainDb = dbFromLinear(metrics.reverbReturnRms) - dbFromLinear(metrics.reverbInputRms);
            finite = finite && metrics.finite && std::isfinite(returnGainDb);
            signal = signal && metrics.reverbInputRms > kSignalFloor && metrics.reverbReturnRms > kSignalFloor;
            minGainDb = any ? std::min(minGainDb, returnGainDb) : returnGainDb;
            maxGainDb = any ? std::max(maxGainDb, returnGainDb) : returnGainDb;
            any = true;

            if (route == 1)
            {
                const double sendGainDb = dbFromLinear(metrics.reverbInputRms) - dbFromLinear(metrics.echoPreTrimRms);
                canonicalSend =
                    canonicalSend && std::abs(sendGainDb - kCanonicalReverbSendGainDb) <= kSendGainToleranceDb;
            }

            auto* routeObject = new juce::DynamicObject();
            routeObject->setProperty("route", kRouteNames[static_cast<size_t>(route)]);
            routeObject->setProperty("input_rms_dbfs", dbFromLinear(metrics.reverbInputRms));
            routeObject->setProperty("return_rms_dbfs", dbFromLinear(metrics.reverbReturnRms));
            routeObject->setProperty("return_gain_db", returnGainDb);
            routeObject->setProperty("input_peak", metrics.reverbInputPeak);
            routeObject->setProperty("return_peak", metrics.reverbReturnPeak);
            routeRows.add(juce::var(routeObject));
        }

        const double spreadDb = maxGainDb - minGainDb;
        worstRouteSpreadDb = std::max(worstRouteSpreadDb, spreadDb);
        engineObject->setProperty("route_gain_spread_db", spreadDb);
        engineObject->setProperty("routes", routeRows);
        engineRows.add(juce::var(engineObject));
    }

    const bool parity = worstRouteSpreadDb <= kMaxRouteSpreadDb;
    root->setProperty("engines", engineRows);
    root->setProperty("worst_route_gain_spread_db", worstRouteSpreadDb);
    root->setProperty("finite", finite);
    root->setProperty("signal_present", signal);
    root->setProperty("canonical_echo_to_reverb_send", canonicalSend);

    const juce::File jsonFile = artifactDirectory().getChildFile("reverb_route_return_parity.json");
    jsonFile.deleteFile();
    const bool wroteJson = jsonFile.replaceWithText(juce::JSON::toString(juce::var(root), true));

    const bool passed = finite && signal && canonicalSend && parity && wroteJson;
    juce::String result;
    result << "ReverbRouteReturnParity: worstRouteSpread=" << juce::String(worstRouteSpreadDb, 2)
           << "dB target<=" << juce::String(kMaxRouteSpreadDb, 1) << "dB finite=" << (finite ? "yes" : "no")
           << " signal=" << (signal ? "yes" : "no") << " canonicalSend=" << (canonicalSend ? "yes" : "no") << "\n"
           << "[INFO] metrics: " << jsonFile.getFullPathName() << "\n"
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

}  // namespace SpatialEchoTests::System

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
