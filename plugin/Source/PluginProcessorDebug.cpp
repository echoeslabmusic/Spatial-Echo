// SpatialEchoAudioProcessor (Debug/Test): Debug harness: audio-thread allocation tracking and the processor-level
//   regression tests the headless runners call.
// Realtime-safety: the allocation-tracking operator new exists only in Debug and headless-test builds; tests run
//   offline.
// See docs/build-and-test.md.

#include "PluginProcessor.h"
#include "ParameterWriteHelpers.h"

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
#include "Tests/Common/AllocationTracking.h"
#include "Tests/Reverb/ReverbProcessorTestAccess.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <new>
#include <thread>

namespace
{
// Same taper as the processor's InputLevel mapping: 0..0.5 -> -60..0 dB,
// 0.5..1 -> 0..+12 dB.
float mapInputLevelToGainForDebug(float raw)
{
    const float clamped = juce::jlimit(0.0f, 1.0f, raw);
    const float db = (clamped <= 0.5f) ? juce::jmap(clamped, 0.0f, 0.5f, -60.0f, 0.0f)
                                       : juce::jmap(clamped, 0.5f, 1.0f, 0.0f, 12.0f);
    return juce::Decibels::decibelsToGain(db);
}

// Allocation tracking state. The replaced operator new below runs on every
// thread, so all three are atomics: tracking is armed with a release store
// after the tracked thread id is published, and operator new reads the flag
// with acquire before comparing the id. The id atomic must be lock-free so
// the allocator path never takes a lock.
std::atomic<int> g_audioThreadAllocs{0};
std::atomic<bool> g_trackAudioAllocs{false};
std::atomic<std::thread::id> g_allocThread{};
static_assert(std::atomic<std::thread::id>::is_always_lock_free, "allocation tracking must stay lock-free");

void countTrackedAllocation() noexcept
{
    if (g_trackAudioAllocs.load(std::memory_order_acquire) &&
        g_allocThread.load(std::memory_order_relaxed) == std::this_thread::get_id())
        g_audioThreadAllocs.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace

void* operator new(std::size_t size)
{
    if (void* p = std::malloc(size))
    {
        countTrackedAllocation();
        return p;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size)
{
    if (void* p = std::malloc(size))
    {
        countTrackedAllocation();
        return p;
    }
    throw std::bad_alloc();
}

void operator delete(void* p) noexcept
{
    std::free(p);
}
void operator delete[](void* p) noexcept
{
    std::free(p);
}

#if defined(__cpp_sized_deallocation)
void operator delete(void* p, std::size_t) noexcept
{
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept
{
    std::free(p);
}
#endif

// Declared in Tests/Common/AllocationTracking.h for the tests in other
// translation units. Tracking counts allocations on the thread that called
// begin, until end.
namespace SpatialEchoDebug
{
void beginAudioAllocationTracking()
{
    g_audioThreadAllocs.store(0, std::memory_order_relaxed);
    g_allocThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
    g_trackAudioAllocs.store(true, std::memory_order_release);
}

int endAudioAllocationTracking()
{
    g_trackAudioAllocs.store(false, std::memory_order_release);
    return g_audioThreadAllocs.load(std::memory_order_relaxed);
}
}  // namespace SpatialEchoDebug

// -------------------------------------------------------------------------
juce::String SpatialEchoAudioProcessor::runSerializationRoundTripTest()
{
    const float fs = 48000.0f;
    const int blkSize = 512;
    const int settleBlocks = 100;
    const int totalSamples = (int)fs;

    juce::MemoryBlock originalState;
    getStateInformation(originalState);

    auto setFloatParam = [&](const char* id, float value)
    { SpatialEchoParameterWrite::setFloatById(treestate, id, value); };
    auto setBoolParam = [&](const char* id, bool value)
    { SpatialEchoParameterWrite::setBoolById(treestate, id, value); };
    auto setChoiceParam = [&](const char* id, int index)
    { SpatialEchoParameterWrite::setChoiceById(treestate, id, index); };

    setFloatParam("Intensity", 0.6f);
    setFloatParam("RepeatRate", 0.4f);
    setFloatParam("Bass", 0.7f);
    setFloatParam("Treble", 0.3f);
    setChoiceParam("DelaySetting", 6);  // Mode 7 (0-based index)
    setFloatParam("Mix", 0.0f);
    setFloatParam("InputLevel", 0.5f);
    setBoolParam("MixLock", false);
    // Noise/wow-flutter are per-flavor internals; the deterministic hooks
    // below feed EchoesModel neutral values directly.
    setBoolParam("EchoSend", true);
    setBoolParam("TempoSync", false);

    applyDelaySettings();

    auto renderOutput = [&](std::vector<float>& out)
    {
        EchoesModel testModel;
        testModel.Reset(fs, 2, blkSize);

        int modeIdx = (DelaySetting != nullptr) ? DelaySetting->getIndex() : 0;
        int heads[3] = {playheadstates[0], playheadstates[1], playheadstates[2]};
        float bass = (Bass != nullptr) ? Bass->load() : 0.7f;
        float treble = (Treble != nullptr) ? Treble->load() : 0.3f;
        float intensity = (Intensity != nullptr) ? Intensity->load() : 0.6f;
        float rate = (RepeatRate != nullptr) ? RepeatRate->load() : 0.4f;
        float inputLevel = (InputLevel != nullptr) ? InputLevel->load() : 0.5f;
        float inputLevelGain = mapInputLevelToGainForDebug(inputLevel);
        float mixValue = (Mix != nullptr) ? Mix->load() : 0.0f;
        const bool mixLocked = (MixLock != nullptr && MixLock->load() > 0.5f);
        if (mixLocked)
            mixValue = 1.0f;
        // Neutral tape character for deterministic regression rendering:
        // noise off, nominal condition, no wow or flutter.
        const bool noiseEnabled = false;
        const float noiseAmt = 0.0f;
        const float condition = 0.3f;
        const float wfDepth = 0.0f;
        const float wowRate = 0.5f;
        const float flutterRate = 0.5f;
        float echoSend = (EchoSend != nullptr) ? EchoSend->load() : 1.0f;

        // Spring settings (engine-native size .65, decay .72, damping .35,
        // modDepth .12, springBalance .5, preDelay .02) expressed as macros.
        ReverbMacroParams reverbMacros;
        reverbMacros.size = 0.65f;
        reverbMacros.decay = 0.72f;
        reverbMacros.tone = 0.69f;
        reverbMacros.mod = 0.12f;
        reverbMacros.character = 0.5f;
        reverbMacros.preDelay = 0.02f;
        reverbMacros.width = 0.0f;
        reverbMacros.engine = 0;

        testModel.UpdateParameters(bass, treble, intensity, rate, heads, delayEnabled[modeIdx], mixValue,
                                   reverbEnabled[modeIdx], inputLevelGain, echoSend, noiseEnabled, noiseAmt, condition,
                                   wfDepth, wowRate, flutterRate, 0, false, 0.0f, 0.0f, 0.0f, false, 0.0f, 0.0f, 1.0f,
                                   100.0f, 150.0f, 2, 0.5f, 0.5f, 0.5f, 0, 0, -1.0f, -1.0f, 1.0f, 1.0f, true, 0, 1.0f,
                                   1.0f, 0.0f, reverbMacros);

        std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));
        for (int w = 0; w < settleBlocks; ++w)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);
        }

        out.assign((size_t)totalSamples, 0.0f);
        int writeIndex = 0;
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        buf[0][0] = 0.9f;
        buf[1][0] = 0.9f;
        testModel.ProcessBuffer(buf, blkSize);
        for (int i = 0; i < blkSize && writeIndex < totalSamples; ++i, ++writeIndex)
            out[(size_t)writeIndex] = buf[0][(size_t)i];

        while (writeIndex < totalSamples)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);
            for (int i = 0; i < blkSize && writeIndex < totalSamples; ++i, ++writeIndex)
                out[(size_t)writeIndex] = buf[0][(size_t)i];
        }
    };

    std::vector<float> before;
    renderOutput(before);

    juce::MemoryBlock savedState;
    getStateInformation(savedState);

    setFloatParam("Intensity", 0.2f);
    setFloatParam("RepeatRate", 0.9f);
    setFloatParam("Bass", 0.2f);
    setFloatParam("Treble", 0.8f);
    setChoiceParam("DelaySetting", 0);
    applyDelaySettings();

    setStateInformation(savedState.getData(), (int)savedState.getSize());
    applyDelaySettings();

    std::vector<float> after;
    renderOutput(after);

    const int len = std::min((int)before.size(), (int)after.size());
    auto normalizedCorrelation = [](const float* a, const float* b, int numSamples)
    {
        if (a == nullptr || b == nullptr || numSamples <= 0)
            return 0.0f;

        double cross = 0.0;
        double energyA = 0.0;
        double energyB = 0.0;
        for (int i = 0; i < numSamples; ++i)
        {
            const double av = a[i];
            const double bv = b[i];
            cross += av * bv;
            energyA += av * av;
            energyB += bv * bv;
        }

        const double denom = std::sqrt(energyA * energyB);
        if (denom <= 1.0e-12)
            return 0.0f;
        return static_cast<float>(cross / denom);
    };
    auto peakAbs = [](const std::vector<float>& data)
    {
        float peak = 0.0f;
        for (float x : data)
            peak = std::max(peak, std::abs(x));
        return peak;
    };

    const float corr = normalizedCorrelation(before.data(), after.data(), len);
    float maxDiff = 0.0f;
    for (int i = 0; i < len; ++i)
        maxDiff = std::max(maxDiff, std::abs(before[(size_t)i] - after[(size_t)i]));
    const float peak = std::max(1e-9f, peakAbs(before));
    const float diffDb = 20.0f * std::log10(std::max(1e-12f, maxDiff / peak));

    setStateInformation(originalState.getData(), (int)originalState.getSize());
    applyDelaySettings();

    const bool passed = (corr >= 0.9999f) && (diffDb <= -80.0f);
    juce::String result;
    result << "Serialize: corr=" << juce::String(corr, 5) << " diff=" << juce::String(diffDb, 1) << " dB "
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String SpatialEchoAudioProcessor::runProcessorTempoMidiUpdateTest()
{
    const double fs = 48000.0;
    const int blkSize = 512;

    prepareToPlay(fs, blkSize);

    struct TestPlayHead final : public juce::AudioPlayHead
    {
        double bpm = 120.0;
        bool playing = true;

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setBpm(bpm);
            info.setIsPlaying(playing);
            juce::AudioPlayHead::TimeSignature timeSig{4, 4};
            info.setTimeSignature(timeSig);
            info.setPpqPosition(0.0);
            return info;
        }
    };

    TestPlayHead testPlayHead;
    setPlayHead(&testPlayHead);

    auto& apvts = getValueTreeState();
    if (apvts.getParameter("TempoSync") == nullptr || apvts.getParameter("Head1NoteDivision") == nullptr ||
        apvts.getParameter(SpatialIDs::binaural::kAzimuth) == nullptr)
        return "ProcessorTempoMidiUpdate: missing params [FAIL]";

    SpatialEchoParameterWrite::setBoolById(apvts, "TempoSync", true);
    SpatialEchoParameterWrite::setFloatById(apvts, SpatialIDs::binaural::kAzimuth, 0.0f);

    const int head1Index = 2;  // quarter note
    SpatialEchoParameterWrite::setChoiceById(apvts, "Head1NoteDivision", head1Index);

    juce::AudioBuffer<float> buffer(2, blkSize);
    juce::MidiBuffer midi;

    testPlayHead.bpm = 120.0;
    buffer.clear();
    buffer.setSample(0, 0, 0.5f);
    buffer.setSample(1, 0, 0.5f);
    EchoesModelTestAccess::enableHeadTimingCapture(echomodel, true);
    processBlock(buffer, midi);
    TapeDelayProcessor::HeadTimingSnapshot snap1;
    if (!EchoesModelTestAccess::getHeadTimingSnapshot(echomodel, snap1))
    {
        buffer.clear();
        buffer.setSample(0, 0, 0.5f);
        buffer.setSample(1, 0, 0.5f);
        processBlock(buffer, midi);
        if (!EchoesModelTestAccess::getHeadTimingSnapshot(echomodel, snap1))
            return "ProcessorTempoMidiUpdate: timing snapshot 1 missing [FAIL]";
    }

    testPlayHead.bpm = 180.0;
    const int settleBlocks = 120;
    for (int i = 0; i < settleBlocks; ++i)
    {
        buffer.clear();
        processBlock(buffer, midi);
    }
    buffer.clear();
    buffer.setSample(0, 0, 0.5f);
    buffer.setSample(1, 0, 0.5f);
    EchoesModelTestAccess::enableHeadTimingCapture(echomodel, true);
    processBlock(buffer, midi);
    TapeDelayProcessor::HeadTimingSnapshot snap2;
    if (!EchoesModelTestAccess::getHeadTimingSnapshot(echomodel, snap2))
    {
        buffer.clear();
        buffer.setSample(0, 0, 0.5f);
        buffer.setSample(1, 0, 0.5f);
        processBlock(buffer, midi);
        if (!EchoesModelTestAccess::getHeadTimingSnapshot(echomodel, snap2))
            return "ProcessorTempoMidiUpdate: timing snapshot 2 missing [FAIL]";
    }

    const float expected1 = getCalculatedSyncedDelayMs(120.0, head1Index);
    const float expected2 = getCalculatedSyncedDelayMs(180.0, head1Index);
    const bool tempoEnabled = snap1.tempoSyncEnabled && snap2.tempoSyncEnabled;
    const bool changed = std::abs(snap2.h1TimeMs - snap1.h1TimeMs) > 10.0f;
    const bool approx1 = (expected1 > 0.0f) ? (std::abs(snap1.h1TimeMs - expected1) / expected1) < 0.2f : false;
    const bool approx2 = (expected2 > 0.0f) ? (std::abs(snap2.h1TimeMs - expected2) / expected2) < 0.2f : false;
    const bool tempoPass = tempoEnabled && changed && approx1 && approx2;

    const auto* binBefore = echomodel.getBinauralProcessor();
    const float beforeAz = (binBefore != nullptr) ? binBefore->getAzimuth() : 0.0f;
    juce::MidiBuffer midiCc;
    midiCc.addEvent(juce::MidiMessage::controllerEvent(1, kMidiCCAzimuth, 100), 0);
    buffer.clear();
    processBlock(buffer, midiCc);
    const auto* binAfter = echomodel.getBinauralProcessor();
    const float afterAz = (binAfter != nullptr) ? binAfter->getAzimuth() : beforeAz;
    const bool midiPass = std::abs(afterAz - beforeAz) > 1.0f;

    juce::String result;
    result << "ProcessorTempoMidiUpdate: tempo=" << (tempoPass ? "OK" : "FAIL") << " H1 "
           << juce::String(snap1.h1TimeMs, 1) << "->" << juce::String(snap2.h1TimeMs, 1) << "ms"
           << " hostBpm=" << juce::String(hostBpm, 1) << " midi=" << (midiPass ? "OK" : "FAIL") << " az "
           << juce::String(beforeAz, 1) << "->" << juce::String(afterAz, 1)
           << (tempoPass && midiPass ? " [PASS]" : " [FAIL]");
    return result;
}

juce::String SpatialEchoAudioProcessor::runPresetCompatibilityTest()
{
    juce::MemoryBlock originalState;
    getStateInformation(originalState);

    SpatialEchoParameterWrite::setFloatById(treestate, "Mix", 0.72f);

    auto stateCopy = treestate.copyState();
    auto valueTree = stateCopy;
    if (auto child = stateCopy.getChildWithName("VALUE_TREE_STATE"); child.isValid())
        valueTree = child;

    // Inject properties that name no current parameter; loading must ignore them.
    valueTree.setProperty("Head1Delay", 123.0f, nullptr);
    valueTree.setProperty("Head2Delay", 234.0f, nullptr);
    valueTree.setProperty("Head3Delay", 345.0f, nullptr);
    valueTree.setProperty("ReverbShake", 1.0f, nullptr);

    std::unique_ptr<juce::XmlElement> xml(stateCopy.createXml());
    if (xml == nullptr)
        return "PresetCompat: XML failure [FAIL]";

    juce::MemoryBlock blob;
    copyXmlToBinary(*xml, blob);
    setStateInformation(blob.getData(), (int)blob.getSize());
    applyDelaySettings();

    auto* mixParam = treestate.getRawParameterValue("Mix");
    const float mixValue = (mixParam != nullptr) ? mixParam->load() : -1.0f;
    const bool mixOk = std::abs(mixValue - 0.72f) < 0.001f;
    const bool removedOk =
        (treestate.getParameter("Head1Delay") == nullptr) && (treestate.getParameter("ReverbShake") == nullptr);

    setStateInformation(originalState.getData(), (int)originalState.getSize());
    applyDelaySettings();

    const bool passed = mixOk && removedOk;
    juce::String result;
    result << "PresetCompat: mix=" << juce::String(mixValue, 3) << " removed=" << (removedOk ? "ok" : "bad") << " "
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String SpatialEchoAudioProcessor::runNoAllocationsTest()
{
    const float fs = 48000.0f;
    const int blkSize = 512;
    const int settleBlocks = 100;
    const int baseBlocks = (int)(fs * 20.0f / blkSize);
    const int cloudBlocks = (int)(fs * 5.0f / blkSize);

    EchoesModel testModel;
    testModel.Reset(fs, 2, blkSize);

    int heads[3] = {1, 0, 0};
    // Spring settings (engine-native size .65, decay .72, damping .35,
    // modDepth .12, springBalance .5, preDelay .02) expressed as macros.
    ReverbMacroParams reverbMacros;
    reverbMacros.size = 0.65f;
    reverbMacros.decay = 0.72f;
    reverbMacros.tone = 0.69f;
    reverbMacros.mod = 0.12f;
    reverbMacros.character = 0.5f;
    reverbMacros.preDelay = 0.02f;
    reverbMacros.width = 0.0f;
    reverbMacros.engine = 0;
    testModel.UpdateParameters(0.5f, 0.5f, 0.3f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.08f, 0.0f, 1.0f,
                               1.0f, 0, false, 0.0f, 0.0f, 0.0f, false, 0.0f, 0.0f, 1.0f, 100.0f, 150.0f, 2, 0.5f, 0.5f,
                               0.5f, 0, 0, -1.0f, -1.0f, 1.0f, 1.0f, true, 0, 1.0f, 1.0f, 0.0f, reverbMacros);

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));
    for (int w = 0; w < settleBlocks; ++w)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        testModel.ProcessBuffer(buf, blkSize);
    }

    using SpatialEchoDebug::beginAudioAllocationTracking;
    using SpatialEchoDebug::endAudioAllocationTracking;

    beginAudioAllocationTracking();
    for (int b = 0; b < baseBlocks; ++b)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        testModel.ProcessBuffer(buf, blkSize);
    }
    const int baseAllocs = endAudioAllocationTracking();

    EchoesModel cloudModel;
    cloudModel.Reset(fs, 2, blkSize);
    int cloudHeads[3] = {0, 0, 0};
    ReverbMacroParams cloudReverbMacros;
    cloudReverbMacros.size = 0.65f;
    cloudReverbMacros.decay = 0.72f;
    cloudReverbMacros.tone = juce::jlimit(0.0f, 1.0f, 1.0f - 0.35f / 1.128f);
    cloudReverbMacros.mod = 0.02f;
    cloudReverbMacros.character = juce::jlimit(0.0f, 1.0f, (0.75f - 0.15f) / 0.70f);
    cloudReverbMacros.preDelay = 0.75f;
    cloudReverbMacros.width = 0.45f;
    cloudReverbMacros.engine = 1;
    cloudModel.UpdateParameters(0.5f, 0.5f, 0.42f, 0.5f, cloudHeads, 0, 1.0f, 1, 0.75f, 1.0f, false, 0.0f, 0.08f, 0.0f,
                                1.0f, 1.0f, 0, false, 0.0f, 0.0f, 0.0f, false, 0.0f, 0.0f, 1.0f, 100.0f, 150.0f, 2,
                                0.5f, 0.5f, 0.5f, 0, 0, -1.0f, -1.0f, 1.0f, 1.0f, true, 0, 1.0f, 1.0f, 0.12f,
                                cloudReverbMacros);
    for (int w = 0; w < settleBlocks; ++w)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        cloudModel.ProcessBuffer(buf, blkSize);
    }

    beginAudioAllocationTracking();
    for (int b = 0; b < cloudBlocks; ++b)
    {
        const float sweep = static_cast<float>(b % 64) / 63.0f;
        ReverbMacroParams sweepReverbMacros;
        sweepReverbMacros.size = 0.45f + 0.25f * sweep;
        sweepReverbMacros.decay = 0.60f + 0.15f * sweep;
        sweepReverbMacros.tone = juce::jlimit(0.0f, 1.0f, 1.0f - (0.25f + 0.35f * sweep) / 1.128f);
        sweepReverbMacros.mod = 0.02f + 0.10f * sweep;
        sweepReverbMacros.character = juce::jlimit(0.0f, 1.0f, (0.55f + 0.35f * sweep - 0.15f) / 0.70f);
        sweepReverbMacros.preDelay = 0.20f + 0.60f * sweep;
        sweepReverbMacros.width = 0.05f + 0.20f * sweep;
        sweepReverbMacros.engine = 1;
        cloudModel.UpdateParameters(0.5f, 0.5f, 0.42f, 0.5f, cloudHeads, 0, 1.0f, 1, 0.75f, 1.0f, false, 0.0f, 0.08f,
                                    0.0f, 1.0f, 1.0f, 0, false, 0.0f, 0.0f, 0.0f, false, 0.0f, 0.0f, 1.0f, 100.0f,
                                    150.0f, 2, 0.5f, 0.5f, 0.5f, 0, 0, -1.0f, -1.0f, 1.0f, 1.0f, true, 0, 1.0f, 1.0f,
                                    0.12f, sweepReverbMacros);

        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        if ((b % 16) == 0)
        {
            buf[0][0] = 0.25f;
            buf[1][0] = -0.15f;
        }
        cloudModel.ProcessBuffer(buf, blkSize);
    }
    const int cloudAllocs = endAudioAllocationTracking();

    // A host can hand the processor a callback larger than the prepared block.
    // ReverbProcessor chunks the audio, but the Spring engine still observes
    // the complete callback size as realtime context. The AutoPanic hard clear
    // must only zero prepared storage; it may not re-prepare or resize it.
    ReverbProcessor hardClearReverb;
    hardClearReverb.Reset(fs, 2, blkSize);
    ReverbMacroParams hardClearMacros;
    hardClearMacros.size = 0.72f;
    hardClearMacros.decay = 0.68f;
    hardClearMacros.preDelay = 1.0f;
    hardClearMacros.engine = 0;
    hardClearReverb.UpdateParameters(1, 0.72f, hardClearMacros, false, 0.0f);
    constexpr int oversizedBlock = 2048;
    std::vector<std::vector<float>> oversized(2, std::vector<float>(static_cast<size_t>(oversizedBlock), 0.0f));
    oversized[0][0] = 0.5f;
    oversized[1][0] = 0.5f;
    hardClearReverb.ProcessBuffer(oversized, oversizedBlock);

    beginAudioAllocationTracking();
    hardClearReverb.hardClearNow();
    const int hardClearAllocs = endAudioAllocationTracking();

    // Engine choice is updated from the host on the audio thread. Cover both
    // complete 50 ms transitions and their hidden drains, including decay
    // shortening and drain completion, so the transition machinery cannot
    // silently regress to allocating work after the initial callbacks.
    ReverbProcessor transitionReverb;
    std::srand(0x5EEDu);
    transitionReverb.Reset(fs, 2, blkSize);
    ReverbMacroParams transitionSpring = hardClearMacros;
    transitionSpring.engine = 0;
    ReverbMacroParams transitionCloud = cloudReverbMacros;
    transitionCloud.engine = 1;
    transitionReverb.UpdateParameters(1, 0.72f, transitionSpring, false, 0.0f);
    std::vector<std::vector<float>> transitionBuffer(2, std::vector<float>(static_cast<size_t>(blkSize), 0.0f));
    constexpr int maxDrainBlocks = 400;  // >2 s at 48 kHz/256; bounded failure guard.

    auto processUntilDrainCompletes = [&]()
    {
        bool enteredDrain = false;
        for (int block = 0; block < maxDrainBlocks; ++block)
        {
            std::fill(transitionBuffer[0].begin(), transitionBuffer[0].end(), 0.0f);
            std::fill(transitionBuffer[1].begin(), transitionBuffer[1].end(), 0.0f);
            transitionReverb.ProcessBuffer(transitionBuffer, blkSize);
            enteredDrain = enteredDrain || ReverbProcessorTestAccess::drainingEngine(transitionReverb) >= 0;
            if (enteredDrain && ReverbProcessorTestAccess::drainingEngine(transitionReverb) < 0)
                return true;
        }
        return false;
    };

    beginAudioAllocationTracking();
    transitionReverb.UpdateParameters(1, 0.72f, transitionCloud, false, 0.0f);
    const bool springDrainCompleted = processUntilDrainCompletes();
    transitionReverb.UpdateParameters(1, 0.72f, transitionSpring, false, 0.0f);
    const bool cloudDrainCompleted = processUntilDrainCompletes();
    const int transitionAllocs = endAudioAllocationTracking();
    const bool drainsCompleted = springDrainCompleted && cloudDrainCompleted;

    const int allocs = baseAllocs + cloudAllocs + hardClearAllocs + transitionAllocs;
    const bool passed =
        (baseAllocs == 0) && (cloudAllocs == 0) && (hardClearAllocs == 0) && (transitionAllocs == 0) && drainsCompleted;

    juce::String result;
    result << "No Allocs: base=" << baseAllocs << " cloud=" << cloudAllocs << " hardClear=" << hardClearAllocs
           << " switchDrain=" << transitionAllocs << " drainsComplete=" << (drainsCompleted ? "yes" : "no")
           << " total=" << allocs << " allocations " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
