// PresetRosterRenders: Render candidate preset parameter sets (plain values, factory-order application semantics)
//   through the real processBlock path from a JSON job spec. Supports test signals, tail capture, a fixed-BPM playhead,
//   MIDI notes, and linear parameter automation lanes. WAVs are untracked voicing artifacts; the manifest JSON is the
//   record.

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "SystemTests.h"
#include "../../PluginProcessor.h"
#include "../../Presets/FactoryPresetLibrary.h"
#include "../Common/TestUtilities.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace SpatialEchoTests::System
{

namespace roster
{

constexpr size_t kMaxJobIdLength = 96;

static bool isAsciiSafeJobId(const juce::String& id, juce::String& reason)
{
    if (id.isEmpty())
    {
        reason = "missing id";
        return false;
    }

    if (static_cast<size_t>(id.getNumBytesAsUTF8()) > kMaxJobIdLength)
    {
        reason = "id too long";
        return false;
    }

    for (int i = 0; i < id.length(); ++i)
    {
        const juce::juce_wchar c = id[i];
        const bool asciiAlphaNum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        const bool asciiPunct = c == '_' || c == '-';
        if (!(asciiAlphaNum || asciiPunct))
        {
            reason = "id must match [A-Za-z0-9_-]{1," + juce::String(static_cast<int>(kMaxJobIdLength)) + "}";
            return false;
        }
    }

    reason.clear();
    return true;
}

static bool resolveLinkedPath(const juce::File& file, juce::File& resolved, int& remainingLinkHops)
{
    // juce::File normalizes dot segments when it constructs the path. Walk
    // from the filesystem root so links in any existing parent component are
    // resolved too; resolving only the final leaf would miss a linked outDir.
    std::vector<juce::String> components;
    auto root = file;

    for (;;)
    {
        const auto parent = root.getParentDirectory();
        if (parent == root)
            break;

        components.push_back(root.getFileName());
        root = parent;
    }

    resolved = root;
    for (auto component = components.rbegin(); component != components.rend(); ++component)
    {
        resolved = resolved.getChildFile(*component);
        if (!resolved.isSymbolicLink())
            continue;

        if (remainingLinkHops-- <= 0)
            return false;

        const auto target = resolved.getLinkedTarget();
        if (target == resolved || !resolveLinkedPath(target, resolved, remainingLinkHops))
            return false;
    }

    return true;
}

static bool safeArtifactChildFile(const juce::File& baseDir, const juce::String& fileName, juce::File& outFile,
                                  juce::String& reason)
{
    if (fileName.isEmpty())
    {
        reason = "empty artifact filename";
        return false;
    }

    if (juce::File::isAbsolutePath(fileName) || fileName.containsAnyOf("/\\") ||
        juce::File(fileName).getFileName() != fileName)
    {
        reason = "artifact filename must be a single path component";
        return false;
    }

    const juce::File candidate = baseDir.getChildFile(fileName);
    juce::File resolvedBase;
    juce::File resolvedCandidate;
    int baseLinkHops = 64;
    int candidateLinkHops = 64;
    if (!resolveLinkedPath(baseDir, resolvedBase, baseLinkHops) ||
        !resolveLinkedPath(candidate, resolvedCandidate, candidateLinkHops))
    {
        reason = "artifact path contains an unresolved symbolic-link cycle";
        return false;
    }

    if (!resolvedCandidate.isAChildOf(resolvedBase))
    {
        reason = "artifact path escapes output directory";
        return false;
    }

    outFile = candidate;
    reason.clear();
    return true;
}

// Deterministic LCG so renders are byte-identical across runs.
struct Rng
{
    uint32_t state;
    explicit Rng(uint32_t seed)
        : state(seed)
    {
    }
    float bipolar()
    {
        state = state * 1664525u + 1013904223u;
        return (static_cast<float>(state >> 8) / 8388608.0f) - 1.0f;
    }
};

// Fixed-tempo transport so tempo-synced jobs can render at non-default BPM.
struct FixedPlayHead : juce::AudioPlayHead
{
    double bpm = 120.0;
    double fs = 48000.0;
    int64_t samplePos = 0;

    juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override
    {
        juce::AudioPlayHead::PositionInfo info;
        info.setBpm(bpm);
        info.setIsPlaying(true);
        info.setTimeInSamples(samplePos);
        const double seconds = static_cast<double>(samplePos) / fs;
        info.setTimeInSeconds(seconds);
        info.setPpqPosition(seconds * bpm / 60.0);
        return info;
    }
};

// One linear plain-value ramp, timeline relative to the first captured sample.
struct AutomationLane
{
    juce::String paramId;
    float fromPlain = 0.0f;
    float toPlain = 0.0f;
    double startSeconds = 0.0;
    double endSeconds = 0.0;
};

struct MidiNoteEvent
{
    double timeSeconds = 0.0;
    int note = 60;
    int velocity = 100;
    double durationSeconds = 0.2;
};

// Short (8 ms) band-limited click: sharp enough for ±2 ms onset gates, no
// intra-burst envelope wiggle for the detector to fire on.
static void addFilteredBurst(std::vector<float>& out, int start, float fs, float gain, uint32_t seed)
{
    Rng rng(seed);
    const int len = static_cast<int>(0.008f * fs);
    const int n = static_cast<int>(out.size());
    float lp = 0.0f;
    const float lpCoeff = std::exp(-2.0f * juce::MathConstants<float>::pi * 3500.0f / fs);
    for (int i = 0; i < len && start + i < n; ++i)
    {
        const float env = std::exp(-static_cast<float>(i) / (0.002f * fs));
        const float noise = rng.bipolar();
        lp = lpCoeff * lp + (1.0f - lpCoeff) * noise;
        out[static_cast<size_t>(start + i)] += gain * env * lp * 3.0f;
    }
}

// Input is active for [0, signalSeconds); the buffer may extend further
// (tail capture) and stays silent there. Peak-normalized to -6 dBFS.
static bool generateSignal(const juce::String& type, std::vector<float>& out, float fs, double signalSeconds,
                           double clickIntervalSeconds)
{
    std::fill(out.begin(), out.end(), 0.0f);
    const int activeSamples = std::max(0, std::min(static_cast<int>(out.size()), static_cast<int>(signalSeconds * fs)));

    if (type == "clicks")
    {
        const int interval = std::max(1, static_cast<int>(clickIntervalSeconds * fs));
        const int firstStart = static_cast<int>(0.5f * fs);
        int burstIndex = 0;
        for (int start = firstStart; start < activeSamples; start += interval, ++burstIndex)
            addFilteredBurst(out, start, fs, 0.9f, 0xBA5E1u + static_cast<uint32_t>(burstIndex));
    }
    else if (type == "hidden-rhythm")
    {
        // Two-beat input pattern 11000011. At a sixteenth-note grid step,
        // a quarter-note echo supplies the complementary 00111100 pattern
        // after startup. This is a controlled grouping stimulus, not evidence
        // that a particular listener can hear the component rhythm.
        const int interval = std::max(1, static_cast<int>(clickIntervalSeconds * fs));
        const int firstStart = static_cast<int>(0.5f * fs);
        int slot = 0;
        for (int start = firstStart; start < activeSamples; start += interval, ++slot)
            if (slot % 8 == 0 || slot % 8 == 1 || slot % 8 == 6 || slot % 8 == 7)
                addFilteredBurst(out, start, fs, 0.9f, 0xBA5E1u + static_cast<uint32_t>(slot));
        // A boundary-straddling burst must not extend the declared input
        // into the silent tail used to measure echo and reverb decay.
        std::fill(out.begin() + activeSamples, out.end(), 0.0f);
    }
    else if (type == "tone")
    {
        // Harmonic-rich tone (saw-like, 12 partials) at 220.13 Hz.
        const float f0 = 220.13f;
        for (int h = 1; h <= 12; ++h)
        {
            const float amp = 0.35f / static_cast<float>(h);
            const float inc = 2.0f * juce::MathConstants<float>::pi * f0 * static_cast<float>(h) / fs;
            float phase = 0.0f;
            for (int i = 0; i < activeSamples; ++i)
            {
                out[static_cast<size_t>(i)] += amp * std::sin(phase);
                phase += inc;
                if (phase >= 2.0f * juce::MathConstants<float>::pi)
                    phase -= 2.0f * juce::MathConstants<float>::pi;
            }
        }
    }
    else if (type == "sine997")
    {
        // C3 parity-probe signal: pure 997 Hz sine at -12 dBFS (no
        // normalization). Harmonic-rich tones ring the spring tank into
        // resonance and are unusable for engine level parity.
        const float inc = 2.0f * juce::MathConstants<float>::pi * 997.0f / fs;
        float phase = 0.0f;
        for (int i = 0; i < activeSamples; ++i)
        {
            out[static_cast<size_t>(i)] = 0.251189f * std::sin(phase);
            phase += inc;
            if (phase >= 2.0f * juce::MathConstants<float>::pi)
                phase -= 2.0f * juce::MathConstants<float>::pi;
        }
        return true;  // skip peak normalization: level IS the point
    }
    else if (type == "noise")
    {
        Rng rng(0x5EED0u);
        float lp = 0.0f;
        const float lpCoeff = std::exp(-2.0f * juce::MathConstants<float>::pi * 9000.0f / fs);
        for (int i = 0; i < activeSamples; ++i)
        {
            lp = lpCoeff * lp + (1.0f - lpCoeff) * rng.bipolar();
            out[static_cast<size_t>(i)] = lp * 2.5f;
        }
    }
    else if (type == "burst")
    {
        // Single 0.5 s noise burst at t = 0.5 s, then silence: tail evidence.
        Rng rng(0xB0057u);
        const int start = static_cast<int>(0.5f * fs);
        const int len = static_cast<int>(0.5f * fs);
        float lp = 0.0f;
        const float lpCoeff = std::exp(-2.0f * juce::MathConstants<float>::pi * 6000.0f / fs);
        for (int i = 0; i < len && start + i < activeSamples; ++i)
        {
            lp = lpCoeff * lp + (1.0f - lpCoeff) * rng.bipolar();
            const float fadeIn = std::min(1.0f, static_cast<float>(i) / (0.01f * fs));
            const float fadeOut = std::min(1.0f, static_cast<float>(len - i) / (0.01f * fs));
            out[static_cast<size_t>(start + i)] = lp * 2.5f * fadeIn * fadeOut;
        }
    }
    else if (type == "impulse")
    {
        const int start = static_cast<int>(0.5f * fs);
        if (start < activeSamples)
            out[static_cast<size_t>(start)] = 1.0f;
    }
    else
    {
        return false;
    }

    const float peak = TestUtilities::peakAbs(out.data(), static_cast<int>(out.size()));
    if (peak > 0.0f)
    {
        const float g = TestUtilities::dbToLinear(-6.0f) / peak;
        for (float& s : out)
            s *= g;
    }
    return true;
}

static bool writeWavFloat(const juce::File& file, const std::vector<float>& left, const std::vector<float>& right,
                          double fs)
{
    file.deleteFile();
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
    const auto writer = format.createWriterFor(
        stream,
        juce::AudioFormatWriterOptions{}.withSampleRate(fs).withNumChannels(2).withBitsPerSample(32).withSampleFormat(
            juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    if (writer == nullptr)
        return false;

    juce::AudioBuffer<float> buf(2, static_cast<int>(left.size()));
    for (int i = 0; i < static_cast<int>(left.size()); ++i)
    {
        buf.setSample(0, i, left[static_cast<size_t>(i)]);
        buf.setSample(1, i, right[static_cast<size_t>(i)]);
    }
    writer->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
    return true;
}

static double r2(double v)
{
    return std::isfinite(v) ? std::round(v * 100.0) / 100.0 : -999.0;
}

}  // namespace roster

juce::String runPresetRosterRenders(SpatialEchoAudioProcessor& processor)
{
    using namespace roster;

    const auto specPath = juce::SystemStats::getEnvironmentVariable("SPATIAL_ECHO_ROSTER_SPEC", {});
    if (specPath.isEmpty())
        return "PresetRosterRenders: SPATIAL_ECHO_ROSTER_SPEC not set [SKIP]";

    const juce::File specFile = juce::File::isAbsolutePath(specPath)
                                    ? juce::File(specPath)
                                    : juce::File::getCurrentWorkingDirectory().getChildFile(specPath);
    if (!specFile.existsAsFile())
        return "PresetRosterRenders\n[FAIL] spec file not found: " + specFile.getFullPathName();

    const auto spec = juce::JSON::parse(specFile.loadFileAsString());
    const auto* jobs = spec.getProperty("jobs", {}).getArray();
    if (jobs == nullptr || jobs->isEmpty())
        return "PresetRosterRenders\n[FAIL] spec has no jobs array";

    const juce::File outDir = SpatialEchoTests::artifactDirectory().getChildFile("preset-roster");
    if (!(outDir.isDirectory() || outDir.createDirectory()))
        return "PresetRosterRenders\n[FAIL] could not create output directory: " + outDir.getFullPathName();

    auto& vts = processor.getValueTreeState();
    bool anyFailure = false;
    juce::Array<juce::var> manifestRows;

    // Plain-value application with loadFactoryPreset semantics: factory
    // parameter order, PreampPreset/TapeFlavor selectors last.
    auto applyPlain = [&vts](const juce::String& paramId, float plainValue) -> bool
    {
        if (auto* p = vts.getParameter(paramId))
        {
            p->setValueNotifyingHost(p->convertTo0to1(plainValue));
            return true;
        }
        return false;
    };

    for (const auto& jobVar : *jobs)
    {
        const auto id = jobVar.getProperty("id", "").toString();
        const double fs = static_cast<double>(jobVar.getProperty("sample_rate", 48000));
        const double signalSeconds = static_cast<double>(jobVar.getProperty("signal_seconds", 8.0));
        const double tailSeconds = static_cast<double>(jobVar.getProperty("tail_seconds", 0.0));
        const double preRollSeconds = static_cast<double>(jobVar.getProperty("pre_roll_seconds", 1.0));
        const double clickInterval = static_cast<double>(jobVar.getProperty("click_interval_seconds", 2.0));
        const auto signalType = jobVar.getProperty("signal", "clicks").toString();
        // Left-only input: ping-pong stereo behavior is invisible with a
        // dual-mono feed (both delay lines carry identical content).
        const bool leftOnly = static_cast<bool>(jobVar.getProperty("input_left_only", false));
        const int blockSize = 512;

        auto* row = new juce::DynamicObject();
        row->setProperty("id", id);
        row->setProperty("signal", signalType);
        row->setProperty("sample_rate", fs);

        juce::String idError;
        if (!isAsciiSafeJobId(id, idError))
        {
            row->setProperty("error", idError);
            anyFailure = true;
            manifestRows.add(juce::var(row));
            continue;
        }

        const int captureSamples = static_cast<int>((signalSeconds + tailSeconds) * fs);
        const int preRollSamples = static_cast<int>(preRollSeconds * fs);

        std::vector<float> input(static_cast<size_t>(captureSamples), 0.0f);
        if (!generateSignal(signalType, input, static_cast<float>(fs), signalSeconds, clickInterval))
        {
            row->setProperty("error", "unknown signal type");
            anyFailure = true;
            manifestRows.add(juce::var(row));
            continue;
        }

        // Fresh state, INIT baseline, then the job's plain values.
        FixedPlayHead playHead;
        const bool hasBpm = jobVar.hasProperty("bpm");
        if (hasBpm)
        {
            playHead.bpm = static_cast<double>(jobVar.getProperty("bpm", 120.0));
            playHead.fs = fs;
            playHead.samplePos = 0;
            processor.setPlayHead(&playHead);
        }

        processor.prepareToPlay(fs, blockSize);
        processor.loadDefaultPreset();

        const auto paramsVar = jobVar.getProperty("params", {});
        juce::StringArray unknownParams;
        int appliedCount = 0;
        if (auto* paramsObj = paramsVar.getDynamicObject())
        {
            auto applyIfPresent = [&](const char* paramId)
            {
                const juce::Identifier key(paramId);
                if (!paramsObj->hasProperty(key))
                    return;
                if (applyPlain(paramId, static_cast<float>(static_cast<double>(paramsObj->getProperty(key)))))
                    ++appliedCount;
                else
                    unknownParams.add(paramId);
            };

            for (const auto* paramId : FactoryPresetLibrary::kParameterIds)
                if (std::strcmp(paramId, "PreampPreset") != 0 && std::strcmp(paramId, "TapeFlavor") != 0)
                    applyIfPresent(paramId);
            applyIfPresent("PreampPreset");
            applyIfPresent("TapeFlavor");

            // Spec keys outside the factory schema are applied last and reported.
            for (const auto& prop : paramsObj->getProperties())
            {
                const auto name = prop.name.toString();
                const bool inFactoryOrder =
                    std::any_of(FactoryPresetLibrary::kParameterIds.begin(), FactoryPresetLibrary::kParameterIds.end(),
                                [&name](const char* pid) { return name == pid; });
                if (inFactoryOrder)
                    continue;
                if (applyPlain(name, static_cast<float>(static_cast<double>(prop.value))))
                {
                    ++appliedCount;
                    unknownParams.add(name + " (non-factory)");
                }
                else
                {
                    unknownParams.add(name);
                }
            }
        }

        // Automation lanes and MIDI notes, capture-relative timelines.
        std::vector<AutomationLane> lanes;
        if (const auto* laneArray = jobVar.getProperty("automation", {}).getArray())
        {
            for (const auto& laneVar : *laneArray)
            {
                AutomationLane lane;
                lane.paramId = laneVar.getProperty("param", "").toString();
                lane.fromPlain = static_cast<float>(static_cast<double>(laneVar.getProperty("from", 0.0)));
                lane.toPlain = static_cast<float>(static_cast<double>(laneVar.getProperty("to", 0.0)));
                lane.startSeconds = static_cast<double>(laneVar.getProperty("start", 0.0));
                lane.endSeconds = static_cast<double>(laneVar.getProperty("end", 0.0));
                if (lane.paramId.isNotEmpty() && lane.endSeconds > lane.startSeconds)
                {
                    applyPlain(lane.paramId, lane.fromPlain);
                    lanes.push_back(lane);
                }
            }
        }

        std::vector<MidiNoteEvent> midiNotes;
        if (const auto* noteArray = jobVar.getProperty("midi_notes", {}).getArray())
        {
            for (const auto& noteVar : *noteArray)
            {
                MidiNoteEvent ev;
                ev.timeSeconds = static_cast<double>(noteVar.getProperty("time", 0.0));
                ev.note = static_cast<int>(noteVar.getProperty("note", 60));
                ev.velocity = static_cast<int>(noteVar.getProperty("velocity", 100));
                ev.durationSeconds = static_cast<double>(noteVar.getProperty("duration", 0.2));
                midiNotes.push_back(ev);
            }
        }

        juce::AudioBuffer<float> block(2, blockSize);
        juce::MidiBuffer midi;
        std::vector<float> outL, outR;
        outL.reserve(static_cast<size_t>(captureSamples));
        outR.reserve(static_cast<size_t>(captureSamples));

        for (int done = 0; done < preRollSamples + captureSamples; done += blockSize)
        {
            const int blockStartCapture = done - preRollSamples;
            const double blockStartSec = static_cast<double>(blockStartCapture) / fs;

            for (const auto& lane : lanes)
            {
                const double t =
                    juce::jlimit(lane.startSeconds, lane.endSeconds, std::max(blockStartSec, lane.startSeconds));
                const double frac = (t - lane.startSeconds) / (lane.endSeconds - lane.startSeconds);
                if (blockStartSec >= lane.startSeconds)
                    applyPlain(lane.paramId,
                               lane.fromPlain + static_cast<float>(frac) * (lane.toPlain - lane.fromPlain));
            }

            midi.clear();
            for (const auto& ev : midiNotes)
            {
                const int onSample = static_cast<int>(ev.timeSeconds * fs) - blockStartCapture;
                if (onSample >= 0 && onSample < blockSize)
                    midi.addEvent(juce::MidiMessage::noteOn(1, ev.note, static_cast<juce::uint8>(ev.velocity)),
                                  onSample);
                const int offSample = static_cast<int>((ev.timeSeconds + ev.durationSeconds) * fs) - blockStartCapture;
                if (offSample >= 0 && offSample < blockSize)
                    midi.addEvent(juce::MidiMessage::noteOff(1, ev.note), offSample);
            }

            block.clear();
            for (int s = 0; s < blockSize; ++s)
            {
                const int idx = blockStartCapture + s;
                const float v = idx >= 0 && idx < captureSamples ? input[static_cast<size_t>(idx)] : 0.0f;
                block.setSample(0, s, v);
                block.setSample(1, s, leftOnly ? 0.0f : v);
            }

            processor.processBlock(block, midi);
            playHead.samplePos += blockSize;

            for (int s = 0; s < blockSize; ++s)
            {
                const int idx = blockStartCapture + s;
                if (idx >= 0 && idx < captureSamples)
                {
                    outL.push_back(block.getSample(0, s));
                    outR.push_back(block.getSample(1, s));
                }
            }
        }

        if (hasBpm)
            processor.setPlayHead(nullptr);

        bool finite = true;
        for (float v : outL)
            if (!std::isfinite(v))
            {
                finite = false;
                break;
            }
        for (float v : outR)
            if (!std::isfinite(v))
            {
                finite = false;
                break;
            }

        const juce::String wavName = id + ".wav";
        juce::File wavFile;
        juce::String wavPathError;
        const bool wavPathOk = safeArtifactChildFile(outDir, wavName, wavFile, wavPathError);
        const bool wavOk = wavPathOk && writeWavFloat(wavFile, outL, outR, fs);
        anyFailure = anyFailure || !finite || !wavOk;

        row->setProperty("wav", wavName);
        row->setProperty("finite", finite);
        row->setProperty("wav_written", wavOk);
        if (!wavPathOk)
            row->setProperty("error", wavPathError);
        row->setProperty("params_applied", appliedCount);
        if (!unknownParams.isEmpty())
            row->setProperty("param_notes", unknownParams.joinIntoString("; "));
        if (hasBpm)
            row->setProperty("bpm", playHead.bpm);
        row->setProperty("peak_dbfs_l",
                         r2(TestUtilities::linearToDb(std::max(1.0e-12f, TestUtilities::peakAbs(outL)))));
        row->setProperty("rms_dbfs_l",
                         r2(TestUtilities::linearToDb(
                             std::max(1.0e-12f, TestUtilities::rmsAbs(outL.data(), static_cast<int>(outL.size()))))));
        manifestRows.add(juce::var(row));
    }

    auto* root = new juce::DynamicObject();
    root->setProperty("test", "PresetRosterRenders");
    root->setProperty("schema", "spatial-echo.preset-roster-renders.v1");
    root->setProperty("spec", specFile.getFullPathName());
    root->setProperty("deterministic_env", juce::SystemStats::getEnvironmentVariable("SPATIAL_ECHO_DETERMINISTIC", ""));
    root->setProperty("jobs", manifestRows);
    root->setProperty("any_failure", anyFailure);

    juce::File manifestFile;
    juce::String manifestPathError;
    if (!safeArtifactChildFile(outDir, "manifest.json", manifestFile, manifestPathError))
    {
        anyFailure = true;
    }
    else
    {
        manifestFile.deleteFile();
        if (!manifestFile.replaceWithText(juce::JSON::toString(juce::var(root), false)))
            anyFailure = true;
    }

    if (!manifestPathError.isEmpty())
        anyFailure = true;

    juce::String result;
    result << "PresetRosterRenders\n[INFO] renders: " << outDir.getFullPathName()
           << "\n[INFO] manifest: " << manifestFile.getFullPathName() << "\n"
           << (anyFailure ? "[FAIL] job failure, non-finite output, or write failure" : "[PASS]");
    return result;
}

}  // namespace SpatialEchoTests::System

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
