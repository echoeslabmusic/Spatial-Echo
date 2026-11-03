// StatePresetTests: Processor state, user and factory presets, preset loading and default-state contracts.

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "SystemTests.h"
#include "../Common/TestUtilities.h"
#include "../../PluginProcessor.h"
#include "../../ParameterWriteHelpers.h"
#include "../../DSP/BinauralProcessor.h"
#include "../../DSP/TapeFlavors.h"
#include "../../Presets/FactoryPresetLibrary.h"
#include "../../Presets/FactoryPresetDescriptions.h"
#include "../../Presets/FactoryPresetBrowseGroups.h"
#include "../../UI/Components/SpatialPresetCatalog.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <tuple>
#include <vector>

namespace SpatialEchoTests::System
{

juce::String runUserPresetPersistenceTest(SpatialEchoAudioProcessor& processor)
{
    const juce::String presetName = "HeadlessPresetPersistence";
    const auto presetFile = processor.getUserPresetDirectoryOnDisk().getChildFile(presetName + ".spatialEchopreset");
    if (presetFile.existsAsFile())
        presetFile.deleteFile();

    auto& vts = processor.getValueTreeState();
    auto setFloatParam = [&](const char* id, float value)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(vts.getParameter(id)))
            p->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, value));
    };
    auto setChoiceParam = [&](const char* id, int index)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter(id)))
        {
            const int maxIdx = std::max(0, p->choices.size() - 1);
            const int clamped = std::max(0, std::min(index, maxIdx));
            const float norm = (maxIdx > 0) ? static_cast<float>(clamped) / static_cast<float>(maxIdx) : 0.0f;
            p->setValueNotifyingHost(norm);
        }
    };
    auto setBoolParam = [&](const char* id, bool value)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(vts.getParameter(id)))
            p->setValueNotifyingHost(value ? 1.0f : 0.0f);
    };

    setFloatParam("Mix", 0.37f);
    setFloatParam("Intensity", 0.82f);
    setChoiceParam("DelaySetting", 6);
    setBoolParam("TapeNoiseEnabled", false);
    const bool saveSucceeded = processor.saveUserPreset(presetName);

    const bool fileWritten = presetFile.existsAsFile();

    SpatialEchoAudioProcessor probe;
    probe.prepareToPlay(48000.0, 512);
    auto& probeVts = probe.getValueTreeState();

    auto setProbePlainParam = [&](const char* id, float plainValue)
    {
        if (auto* p = probeVts.getParameter(id))
            p->setValueNotifyingHost(p->convertTo0to1(plainValue));
    };

    probe.loadUserPreset(presetName);

    const float mixLoaded =
        (probeVts.getRawParameterValue("Mix") != nullptr) ? probeVts.getRawParameterValue("Mix")->load() : -1.0f;
    const float intensityLoaded = (probeVts.getRawParameterValue("Intensity") != nullptr)
                                      ? probeVts.getRawParameterValue("Intensity")->load()
                                      : -1.0f;
    const auto* delaySettingChoice = dynamic_cast<juce::AudioParameterChoice*>(probeVts.getParameter("DelaySetting"));
    const int delayModeLoaded = (delaySettingChoice != nullptr) ? delaySettingChoice->getIndex() : -1;
    const bool tapeNoiseDisabled = (probeVts.getRawParameterValue("TapeNoiseEnabled") != nullptr) &&
                                   probeVts.getRawParameterValue("TapeNoiseEnabled")->load() <= 0.5f;
    const bool listedInProbe = probe.getUserPresetNames().contains(presetName);

    const juce::String renamedName = presetName + "_Renamed";
    const auto renamedFile = probe.getUserPresetDirectoryOnDisk().getChildFile(renamedName + ".spatialEchopreset");
    if (renamedFile.existsAsFile())
        renamedFile.deleteFile();

    // A directory at the destination forces the final file replacement to
    // fail without relying on user permissions or a full filesystem.
    const auto blockedName = "HeadlessStorageFailure_" + juce::Uuid().toString();
    const auto blockedDestination =
        probe.getUserPresetDirectoryOnDisk().getChildFile(blockedName + ".spatialEchopreset");
    const auto blocker = blockedDestination.getChildFile("keep");
    const bool failureFixtureReady = blockedDestination.createDirectory().wasOk() && blocker.replaceWithText("keep");
    const auto sourceBeforeFailure = presetFile.loadFileAsString();
    const bool failedRenamePreservesSource = failureFixtureReady && !probe.renameUserPreset(presetName, blockedName) &&
                                             presetFile.existsAsFile() &&
                                             presetFile.loadFileAsString() == sourceBeforeFailure;
    const bool failedSaveReported = failureFixtureReady && !probe.saveUserPreset(blockedName);
    const bool failedExportReported =
        failureFixtureReady && !probe.exportUserPresetToFile(blockedDestination, blockedName);
    juce::TemporaryFile importFile(artifactDirectory().getChildFile("import.spatialEchopreset"));
    const bool importFixtureReady = probe.exportUserPresetToFile(importFile.getFile(), blockedName);
    setProbePlainParam("Mix", 0.19f);
    juce::MemoryBlock stateBeforeFailedImport;
    probe.getStateInformation(stateBeforeFailedImport);
    juce::String importedName = "unchanged";
    const bool failedImportReported = failureFixtureReady && importFixtureReady &&
                                      !probe.importUserPresetFromFile(importFile.getFile(), &importedName);
    juce::MemoryBlock stateAfterFailedImport;
    probe.getStateInformation(stateAfterFailedImport);
    const bool failedImportPreservesState =
        failedImportReported && importedName == "unchanged" && stateBeforeFailedImport == stateAfterFailedImport;
    const bool failureCleanup = blocker.deleteFile() && blockedDestination.deleteFile();
    const bool storageFailuresSafe = failedRenamePreservesSource && failedSaveReported && failedExportReported &&
                                     failedImportPreservesState && failureCleanup;

    const bool importSucceeded = failureCleanup && importFixtureReady &&
                                 probe.importUserPresetFromFile(importFile.getFile(), &importedName) &&
                                 importedName == blockedName && blockedDestination.existsAsFile();
    const auto* importedMix = probeVts.getRawParameterValue("Mix");
    const bool importApplied =
        importSucceeded && importedMix != nullptr && std::abs(importedMix->load() - mixLoaded) <= 0.001f;
    setProbePlainParam("Mix", 0.61f);
    probe.loadUserPreset(blockedName);
    const bool importPersists =
        importApplied && importedMix != nullptr && std::abs(importedMix->load() - mixLoaded) <= 0.001f;
    const bool importCleanup = probe.deleteUserPreset(blockedName);

    const bool renameOk = probe.renameUserPreset(presetName, renamedName);
    const bool renameListed = probe.getUserPresetNames().contains(renamedName);
    const bool renameFileExists = renamedFile.existsAsFile();
    const bool deleteOk = probe.deleteUserPreset(renamedName);
    const bool deleteListed = probe.getUserPresetNames().contains(renamedName);
    const bool deleteFileGone = !renamedFile.existsAsFile();

    const bool mixOk = std::abs(mixLoaded - 0.37f) <= 0.01f;
    const bool intensityOk = std::abs(intensityLoaded - 0.82f) <= 0.01f;
    const bool modeOk = (delayModeLoaded == 6);
    const bool manageOk = renameOk && renameListed && renameFileExists && deleteOk && !deleteListed && deleteFileGone;
    const bool passed = saveSucceeded && fileWritten && mixOk && intensityOk && modeOk && listedInProbe && manageOk &&
                        tapeNoiseDisabled && storageFailuresSafe && importPersists && importCleanup;

    if (presetFile.existsAsFile())
        presetFile.deleteFile();
    if (renamedFile.existsAsFile())
        renamedFile.deleteFile();

    juce::String result;
    result << "UserPresetPersistence: file=" << (fileWritten ? "yes" : "no")
           << " listed=" << (listedInProbe ? "yes" : "no") << " rename=" << (renameOk ? "yes" : "no")
           << " delete=" << (deleteOk ? "yes" : "no") << " mix=" << juce::String(mixLoaded, 3)
           << " int=" << juce::String(intensityLoaded, 3) << " mode=" << delayModeLoaded
           << " noise=" << (tapeNoiseDisabled ? "off" : "on")
           << " storageFailures=" << (storageFailuresSafe ? "safe" : "FAIL")
           << " import=" << (importPersists ? "persisted" : "FAIL") << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runUserPresetSofaPathPolicyTest(SpatialEchoAudioProcessor& processor)
{
    // Version 1.0.0 presets and sessions can carry a SofaPath root property.
    // Loading either must ignore it: the embedded HRTF stays loaded and the
    // property does not re-enter the processor state.
    const juce::String presetName = "HeadlessSofaPathPolicy";
    const auto presetFile = processor.getUserPresetDirectoryOnDisk().getChildFile(presetName + ".spatialEchopreset");
    if (presetFile.existsAsFile())
        presetFile.deleteFile();

    processor.prepareToPlay(48000.0, 512);
    auto* binaural = processor.getBinauralProcessor();
    if (binaural == nullptr)
        return "UserPresetSofaPathPolicy: binaural processor missing [FAIL]";

    const auto missingFile = artifactDirectory().getChildFile("missing_user_preset_policy.sofa");
    if (missingFile.existsAsFile())
        missingFile.deleteFile();

    // Build a preset file and a session blob that carry the property.
    auto& state = processor.getValueTreeState().state;
    const juce::String stalePath = missingFile.getFullPathName();
    state.setProperty("SofaPath", stalePath, nullptr);
    processor.saveUserPreset(presetName);
    juce::MemoryBlock sessionWithPath;
    processor.getStateInformation(sessionWithPath);
    state.removeProperty("SofaPath", nullptr);
    const bool fileWritten = presetFile.existsAsFile() && presetFile.loadFileAsString().contains("SofaPath");
    const bool defaultLoadedBefore = binaural->isHrtfLoaded();

    processor.loadUserPreset(presetName);
    const bool presetPathIgnored = !state.hasProperty("SofaPath");

    processor.setStateInformation(sessionWithPath.getData(), static_cast<int>(sessionWithPath.getSize()));
    const bool sessionPathIgnored = !processor.getValueTreeState().state.hasProperty("SofaPath");
    const bool hrtfLoadedAfter = binaural->isHrtfLoaded();

    if (presetFile.existsAsFile())
        presetFile.deleteFile();

    const bool passed =
        fileWritten && defaultLoadedBefore && presetPathIgnored && sessionPathIgnored && hrtfLoadedAfter;

    juce::String result;
    result << "UserPresetSofaPathPolicy: file=" << (fileWritten ? "yes" : "no")
           << " default=" << (defaultLoadedBefore ? "loaded" : "missing")
           << " presetIgnored=" << (presetPathIgnored ? "yes" : "no")
           << " sessionIgnored=" << (sessionPathIgnored ? "yes" : "no")
           << " hrtf=" << (hrtfLoadedAfter ? "loaded" : "missing") << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runMixLockStateRecallAuthorityTest(SpatialEchoAudioProcessor& processor)
{
    juce::ignoreUnused(processor);

    constexpr double fs = 48000.0;
    static constexpr int blockSize = 256;  // static: read inside captureless lambdas (MSVC)
    constexpr int settleBlocks = 24;
    constexpr int captureBlocks = 48;

    auto setFloatParam = [](SpatialEchoAudioProcessor& target, const char* id, float plainValue)
    {
        auto& vts = target.getValueTreeState();
        if (auto* p = vts.getParameter(id))
            p->setValueNotifyingHost(p->convertTo0to1(plainValue));
    };
    auto setBoolParam = [](SpatialEchoAudioProcessor& target, const char* id, bool value)
    {
        auto& vts = target.getValueTreeState();
        if (auto* p = vts.getParameter(id))
            p->setValueNotifyingHost(value ? 1.0f : 0.0f);
    };
    auto setChoiceParam = [](SpatialEchoAudioProcessor& target, const char* id, int index)
    {
        auto& vts = target.getValueTreeState();
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter(id)))
        {
            const int maxIdx = std::max(0, p->choices.size() - 1);
            const int clamped = std::max(0, std::min(index, maxIdx));
            const float norm = (maxIdx > 0) ? static_cast<float>(clamped) / static_cast<float>(maxIdx) : 0.0f;
            p->setValueNotifyingHost(norm);
        }
    };

    auto configureAudibleState = [&](SpatialEchoAudioProcessor& target, float mix, bool mixLock)
    {
        target.prepareToPlay(fs, blockSize);
        setChoiceParam(target, "DelaySetting", 11);  // reverb-only: wet/dry contrast is immediate.
        setFloatParam(target, "Mix", mix);
        setBoolParam(target, "MixLock", mixLock);
        setFloatParam(target, "InputLevel", 0.5f);
        setFloatParam(target, "ReverbLevel", 0.0f);
        setFloatParam(target, "OutputGain", 0.5f);
        setBoolParam(target, "SpatialEnabled", false);
    };

    auto processSilence = [](SpatialEchoAudioProcessor& target)
    {
        juce::AudioBuffer<float> buffer(2, blockSize);
        juce::MidiBuffer midi;
        buffer.clear();
        target.processBlock(buffer, midi);
    };

    auto render = [&](SpatialEchoAudioProcessor& target)
    {
        std::vector<float> out;
        out.reserve(static_cast<size_t>(captureBlocks * blockSize * 2));
        juce::MidiBuffer midi;
        float phase = 0.0f;
        const float phaseInc = 2.0f * juce::MathConstants<float>::pi * 220.0f / static_cast<float>(fs);

        for (int b = 0; b < settleBlocks + captureBlocks; ++b)
        {
            juce::AudioBuffer<float> buffer(2, blockSize);
            for (int s = 0; s < blockSize; ++s)
            {
                const float x = std::sin(phase) * 0.08f;
                phase += phaseInc;
                if (phase >= 2.0f * juce::MathConstants<float>::pi)
                    phase -= 2.0f * juce::MathConstants<float>::pi;
                buffer.setSample(0, s, x);
                buffer.setSample(1, s, x);
            }

            target.processBlock(buffer, midi);

            if (b >= settleBlocks)
            {
                for (int s = 0; s < blockSize; ++s)
                {
                    out.push_back(buffer.getSample(0, s));
                    out.push_back(buffer.getSample(1, s));
                }
            }
        }

        return out;
    };

    SpatialEchoAudioProcessor savedSource;
    configureAudibleState(savedSource, 0.20f, true);
    juce::MemoryBlock savedState;
    savedSource.getStateInformation(savedState);

    SpatialEchoAudioProcessor expected;
    expected.prepareToPlay(fs, blockSize);
    expected.setStateInformation(savedState.getData(), static_cast<int>(savedState.getSize()));
    const auto expectedRender = render(expected);

    SpatialEchoAudioProcessor reused;
    configureAudibleState(reused, 0.85f, false);
    processSilence(reused);
    setBoolParam(reused, "MixLock", true);
    processSilence(reused);
    reused.prepareToPlay(fs, blockSize);
    reused.setStateInformation(savedState.getData(), static_cast<int>(savedState.getSize()));
    const auto recalledRender = render(reused);

    double errEnergy = 0.0;
    double refEnergy = 0.0;
    float maxDiff = 0.0f;
    const size_t count = std::min(expectedRender.size(), recalledRender.size());
    int invalid = 0;
    for (size_t i = 0; i < count; ++i)
    {
        const float a = expectedRender[i];
        const float b = recalledRender[i];
        if (!std::isfinite(a) || !std::isfinite(b))
            ++invalid;
        const float d = a - b;
        errEnergy += static_cast<double>(d) * static_cast<double>(d);
        refEnergy += static_cast<double>(a) * static_cast<double>(a);
        maxDiff = std::max(maxDiff, std::abs(d));
    }

    const float errRatio = static_cast<float>(std::sqrt(errEnergy / std::max(refEnergy, 1.0e-20)));
    const float errDb = TestUtilities::linearToDb(errRatio);

    // Saving while Mix Lock holds the dry mix must not adopt an automated
    // underlying wet value, and a host autosave must not reset the held cache.
    reused.loadDefaultPreset();
    configureAudibleState(reused, 0.0f, true);
    setChoiceParam(reused, "DelaySetting", 0);
    setBoolParam(reused, "EchoSend", false);
    setBoolParam(reused, "TapeNoiseEnabled", false);
    setBoolParam(reused, "OutputProtection", false);
    processSilence(reused);  // latch the dry Mix before changing its underlying value
    setFloatParam(reused, "Mix", 1.0f);
    const auto beforeSaveRender = render(reused);
    juce::MemoryBlock lockedState;
    reused.getStateInformation(lockedState);
    const auto afterSaveRender = render(reused);
    const auto rms = [](const std::vector<float>& samples)
    {
        double energy = 0.0;
        for (const auto sample : samples)
            energy += static_cast<double>(sample) * sample;
        return std::sqrt(energy / static_cast<double>(std::max<size_t>(1, samples.size())));
    };
    const double beforeSaveRms = rms(beforeSaveRender);
    const double afterSaveRms = rms(afterSaveRender);
    const double saveGainRatio = afterSaveRms / std::max(1.0e-12, beforeSaveRms);
    const bool savePreservesHeldMix =
        beforeSaveRms > 0.01 && std::isfinite(saveGainRatio) && std::abs(saveGainRatio - 1.0) <= 0.001;
    const bool passed =
        (invalid == 0) && (count > 0) && (errDb <= -60.0f) && (maxDiff <= 0.0015f) && savePreservesHeldMix;

    juce::String result;
    result << "MixLockStateRecallAuthority: err=" << juce::String(errDb, 1) << "dB"
           << " maxDiff=" << juce::String(maxDiff, 6) << " samples=" << static_cast<int>(count)
           << " invalid=" << invalid << " saveGain=" << juce::String(saveGainRatio, 6) << " "
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runSelectorSideEffectStoppedTransportRoundTripTest(SpatialEchoAudioProcessor& processor)
{
    juce::ignoreUnused(processor);

    constexpr double fs = 48000.0;
    constexpr int blockSize = 256;
    constexpr float kTol = 0.0025f;

    auto setFloatParam = [](SpatialEchoAudioProcessor& target, const char* id, float plainValue)
    {
        auto& vts = target.getValueTreeState();
        if (auto* p = vts.getParameter(id))
            p->setValueNotifyingHost(p->convertTo0to1(plainValue));
    };
    auto setChoiceParam = [](SpatialEchoAudioProcessor& target, const char* id, int index)
    {
        auto& vts = target.getValueTreeState();
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter(id)))
        {
            const int maxIdx = std::max(0, p->choices.size() - 1);
            const int clamped = std::max(0, std::min(index, maxIdx));
            const float norm = (maxIdx > 0) ? static_cast<float>(clamped) / static_cast<float>(maxIdx) : 0.0f;
            p->setValueNotifyingHost(norm);
        }
    };
    auto readFloatParam = [](SpatialEchoAudioProcessor& target, const char* id)
    {
        auto* value = target.getValueTreeState().getRawParameterValue(id);
        return value != nullptr ? value->load() : -1000.0f;
    };
    auto readChoiceParam = [](SpatialEchoAudioProcessor& target, const char* id)
    {
        auto* choice = dynamic_cast<juce::AudioParameterChoice*>(target.getValueTreeState().getParameter(id));
        return choice != nullptr ? choice->getIndex() : -1;
    };
    auto absErr = [](float a, float b) { return std::abs(a - b); };

    // Tape flavor, Drive, and the noise override are direct parameters with no
    // hidden side-effect params to materialize. The preamp selector still has
    // apply-on-select side effects; all tape controls must survive a
    // stopped-transport save/load round trip.
    const int wornFlavorIndex = static_cast<int>(TapeFlavor::Worn);

    SpatialEchoAudioProcessor source;
    source.prepareToPlay(fs, blockSize);

    setFloatParam(source, "InputLevel", 0.12f);
    setFloatParam(source, "Bass", 0.10f);
    setFloatParam(source, "Treble", 0.90f);
    setFloatParam(source, "TapeDrive", 0.05f);
    setFloatParam(source, "TapeNoiseEnabled", 0.0f);

    setChoiceParam(source, "PreampPreset", 1);  // Warm: 0.55 / 0.65 / 0.40
    setChoiceParam(source, "TapeFlavor", wornFlavorIndex);

    struct SaveNotificationProbe final : juce::AudioProcessorListener
    {
        void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override { ++notifications; }
        void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails&) override {}
        int notifications = 0;
    } notificationProbe;
    source.addListener(&notificationProbe);
    juce::MemoryBlock state;
    source.getStateInformation(state);
    juce::MemoryBlock repeatedState;
    source.getStateInformation(repeatedState);
    const bool snapshotObservational = notificationProbe.notifications == 0 &&
                                       absErr(readFloatParam(source, "InputLevel"), 0.12f) <= kTol &&
                                       absErr(readFloatParam(source, "Bass"), 0.10f) <= kTol &&
                                       absErr(readFloatParam(source, "Treble"), 0.90f) <= kTol;
    const bool repeatedStateStable = state == repeatedState;

    // Audio-published MIDI must be included without consuming the deferred UI
    // update or calling parameter listeners on the host's save thread.
    juce::AudioBuffer<float> midiBuffer(2, blockSize);
    midiBuffer.clear();
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 21, 100), 0);
    source.processBlock(midiBuffer, midi);
    const float liveAzimuthBeforeSave = readFloatParam(source, "SpatialAzimuth");
    notificationProbe.notifications = 0;
    juce::MemoryBlock midiState;
    source.getStateInformation(midiState);
    const bool pendingSaveObservational =
        notificationProbe.notifications == 0 &&
        juce::exactlyEqual(readFloatParam(source, "SpatialAzimuth"), liveAzimuthBeforeSave);
    source.removeListener(&notificationProbe);
    // The public azimuth parameter has a one-degree step. Both a save made
    // before the deferred flush and the eventual live value must use it.
    const float expectedAzimuth = std::round(juce::jmap(100.0f / 127.0f, -180.0f, 180.0f));
    bool midiSaved = false;
    if (const auto midiXml =
            juce::AudioProcessor::getXmlFromBinary(midiState.getData(), static_cast<int>(midiState.getSize())))
    {
        const auto midiTree = juce::ValueTree::fromXml(*midiXml);
        const auto azimuthState = midiTree.getChildWithProperty("id", "SpatialAzimuth");
        midiSaved = azimuthState.isValid() &&
                    absErr(static_cast<float>(azimuthState.getProperty("value")), expectedAzimuth) <= kTol;
    }
    source.flushPendingHostUpdatesForUi();
    const bool pendingUpdatePreserved = absErr(readFloatParam(source, "SpatialAzimuth"), expectedAzimuth) <= kTol;

    SpatialEchoAudioProcessor probe;
    probe.prepareToPlay(fs, blockSize);
    probe.setStateInformation(state.getData(), static_cast<int>(state.getSize()));

    const int preampLoaded = readChoiceParam(probe, "PreampPreset");
    const int flavorLoaded = readChoiceParam(probe, "TapeFlavor");
    const float inputErr = absErr(readFloatParam(probe, "InputLevel"), 0.55f);
    const float bassErr = absErr(readFloatParam(probe, "Bass"), 0.65f);
    const float trebleErr = absErr(readFloatParam(probe, "Treble"), 0.40f);
    const float driveErr = absErr(readFloatParam(probe, "TapeDrive"), 0.05f);
    const float noiseEnabledErr = absErr(readFloatParam(probe, "TapeNoiseEnabled"), 0.0f);

    // A saved state without a TapeNoiseEnabled child must load that parameter's
    // default. Reload such a state into this already-used instance, which is
    // currently Noise off, and expect Noise on.
    bool legacyNoiseDefaultsEnabled = false;
    if (auto legacyXml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize())))
    {
        bool removed = false;
        for (auto* child : legacyXml->getChildIterator())
        {
            if (child->hasTagName("PARAM") && child->getStringAttribute("id") == "TapeNoiseEnabled")
            {
                legacyXml->removeChildElement(child, true);
                removed = true;
                break;
            }
        }

        juce::MemoryBlock legacyState;
        juce::AudioProcessor::copyXmlToBinary(*legacyXml, legacyState);
        probe.setStateInformation(legacyState.getData(), static_cast<int>(legacyState.getSize()));
        legacyNoiseDefaultsEnabled = removed && absErr(readFloatParam(probe, "TapeNoiseEnabled"), 1.0f) <= kTol;
    }

    const float maxPreampErr = std::max({inputErr, bassErr, trebleErr});
    const bool selectorsOk = (preampLoaded == 1) && (flavorLoaded == wornFlavorIndex);
    const bool valuesOk = maxPreampErr <= kTol && driveErr <= kTol && noiseEnabledErr <= kTol;
    const bool passed = selectorsOk && valuesOk && legacyNoiseDefaultsEnabled && snapshotObservational &&
                        repeatedStateStable && pendingSaveObservational && midiSaved && pendingUpdatePreserved;

    juce::String result;
    result << "SelectorSideEffectStoppedTransportRoundTrip:"
           << " selectors=" << preampLoaded << "/" << flavorLoaded << " preErr=" << juce::String(maxPreampErr, 5)
           << " driveErr=" << juce::String(driveErr, 5) << " noiseErr=" << juce::String(noiseEnabledErr, 5)
           << " legacyNoise=" << (legacyNoiseDefaultsEnabled ? "default" : "stale")
           << " observational=" << (snapshotObservational && pendingSaveObservational ? "yes" : "no")
           << " stable=" << (repeatedStateStable ? "yes" : "no") << " midiSaved=" << (midiSaved ? "yes" : "no")
           << " midiQueued=" << (pendingUpdatePreserved ? "yes" : "no") << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runWholeStateReplacementSuspensionTest(SpatialEchoAudioProcessor& processor)
{
    juce::ignoreUnused(processor);

    constexpr double fs = 48000.0;
    constexpr int blockSize = 256;

    struct SuspensionProbe : juce::ValueTree::Listener
    {
        explicit SuspensionProbe(SpatialEchoAudioProcessor& ownerIn)
            : owner(ownerIn)
        {
        }

        void valueTreeRedirected(juce::ValueTree&) override
        {
            ++redirects;
            if (!owner.isSuspended())
                ++unsuspendedRedirects;
        }

        SpatialEchoAudioProcessor& owner;
        int redirects = 0;
        int unsuspendedRedirects = 0;
    };

    auto setPlain = [](SpatialEchoAudioProcessor& target, const char* id, float plainValue)
    {
        if (auto* p = target.getValueTreeState().getParameter(id))
            p->setValueNotifyingHost(p->convertTo0to1(plainValue));
    };

    auto setChoice = [](SpatialEchoAudioProcessor& target, const char* id, int index)
    {
        auto& vts = target.getValueTreeState();
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter(id)))
        {
            const int maxIdx = std::max(0, p->choices.size() - 1);
            const int clamped = std::max(0, std::min(index, maxIdx));
            p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(clamped)));
        }
    };

    auto makeStateBlock = [&]()
    {
        SpatialEchoAudioProcessor source;
        source.prepareToPlay(fs, blockSize);
        setPlain(source, "Mix", 0.73f);
        setPlain(source, "Intensity", 0.84f);
        setPlain(source, "InputLevel", 0.82f);
        setPlain(source, "OutputGain", 0.62f);
        setChoice(source, "DelaySetting", 7);

        juce::MemoryBlock state;
        source.getStateInformation(state);
        return state;
    };

    auto probeOperation = [&](const juce::String& name, auto&& operation)
    {
        SpatialEchoAudioProcessor target;
        target.prepareToPlay(fs, blockSize);
        setPlain(target, "Mix", 0.11f);
        setPlain(target, "Intensity", 0.25f);
        setChoice(target, "DelaySetting", 0);

        SuspensionProbe probe(target);
        target.getValueTreeState().state.addListener(&probe);
        operation(target);
        target.getValueTreeState().state.removeListener(&probe);

        juce::String summary;
        summary << name << "=" << probe.redirects << "/" << probe.unsuspendedRedirects;
        return std::tuple<juce::String, int, int>(summary, probe.redirects, probe.unsuspendedRedirects);
    };

    const auto stateBlock = makeStateBlock();

    auto [setStateSummary, setStateRedirects, setStateUnsuspended] =
        probeOperation("setState", [&](SpatialEchoAudioProcessor& target)
                       { target.setStateInformation(stateBlock.getData(), static_cast<int>(stateBlock.getSize())); });

    // Host session restore still swaps the whole tree, and must only do so
    // while processing is suspended.
    const bool setStateOk = setStateRedirects > 0 && setStateUnsuspended == 0;

    const juce::String presetName = "WholeStateSuspensionProbe";
    SpatialEchoAudioProcessor presetSource;
    const auto presetFile = presetSource.getUserPresetDirectoryOnDisk().getChildFile(presetName + ".spatialEchopreset");
    if (presetFile.existsAsFile())
        presetFile.deleteFile();

    presetSource.prepareToPlay(fs, blockSize);
    setPlain(presetSource, "Mix", 0.68f);
    setPlain(presetSource, "InputLevel", 0.76f);
    setChoice(presetSource, "DelaySetting", 5);
    presetSource.saveUserPreset(presetName);

    auto [loadUserSummary, loadUserRedirects, loadUserUnsuspended] =
        probeOperation("loadUser", [&](SpatialEchoAudioProcessor& target) { target.loadUserPreset(presetName); });

    if (presetFile.existsAsFile())
        presetFile.deleteFile();

    auto [defaultSummary, defaultRedirects, defaultUnsuspended] =
        probeOperation("default", [&](SpatialEchoAudioProcessor& target) { target.loadDefaultPreset(); });

    // Interactive preset loads must NOT replace the state tree at all:
    // they apply values in-place through the undoable parameter path so the
    // UndoManager history survives (PresetLoadUndoRecovery test) and no
    // tree-redirect race window exists.
    const bool loadUserOk = loadUserRedirects == 0 && loadUserUnsuspended == 0;
    const bool defaultOk = defaultRedirects == 0 && defaultUnsuspended == 0;

    const bool passed = setStateOk && loadUserOk && defaultOk;

    juce::String result;
    result << "WholeStateReplacementSuspension:"
           << " " << setStateSummary << " " << loadUserSummary << " " << defaultSummary << " "
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runHostTailLengthContractTest(SpatialEchoAudioProcessor& processor)
{
    const double tailSeconds = processor.getTailLengthSeconds();
    const bool reportsInfiniteTail = std::isinf(tailSeconds) && tailSeconds > 0.0;

    juce::String result;
    result << "HostTailLengthContract: tail="
           << (std::isinf(tailSeconds) ? juce::String("inf") : juce::String(tailSeconds, 3)) << " selfOscCapable=yes"
           << " " << (reportsInfiniteTail ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runActiveFactoryPresetSwitchContinuityTest(SpatialEchoAudioProcessor& processor)
{
    constexpr double fs = 48000.0;
    constexpr int blockSize = 256;
    constexpr int warmBlocks = 24;
    constexpr int settleBlocksPerPreset = 6;
    constexpr float inputAmp = 0.12f;
    constexpr float maxBoundaryJumpAllowed = 0.8f;
    constexpr float maxAdjacentJumpAllowed = 0.8f;
    constexpr float maxPeakAllowed = 1.25f;

    processor.prepareToPlay(fs, blockSize);

    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    double phase = 0.0;
    const double phaseInc = 2.0 * juce::MathConstants<double>::pi * 330.0 / fs;
    float previousOut[2] = {0.0f, 0.0f};
    bool havePrevious = false;
    float maxBoundaryJump = 0.0f;
    float maxAdjacentJump = 0.0f;
    float maxPeak = 0.0f;
    int invalidSamples = 0;
    juce::String worstBoundaryPreset;
    juce::String worstAdjacentPreset;
    juce::String worstPeakPreset;

    auto renderBlock = [&](const juce::String& presetName, bool measureBoundary)
    {
        for (int s = 0; s < blockSize; ++s)
        {
            const float sample = inputAmp * static_cast<float>(std::sin(phase));
            phase += phaseInc;
            if (phase >= 2.0 * juce::MathConstants<double>::pi)
                phase -= 2.0 * juce::MathConstants<double>::pi;

            buffer.setSample(0, s, sample);
            buffer.setSample(1, s, sample * 0.85f);
        }

        processor.processBlock(buffer, midi);

        for (int ch = 0; ch < 2; ++ch)
        {
            float prev = havePrevious ? previousOut[ch] : buffer.getSample(ch, 0);
            for (int s = 0; s < blockSize; ++s)
            {
                const float y = buffer.getSample(ch, s);
                if (!std::isfinite(y))
                {
                    ++invalidSamples;
                    continue;
                }

                const float absY = std::abs(y);
                if (absY > maxPeak)
                {
                    maxPeak = absY;
                    worstPeakPreset = presetName;
                }

                const float delta = std::abs(y - prev);
                if (s == 0 && measureBoundary && havePrevious && delta > maxBoundaryJump)
                {
                    maxBoundaryJump = delta;
                    worstBoundaryPreset = presetName;
                }
                if (havePrevious && delta > maxAdjacentJump)
                {
                    maxAdjacentJump = delta;
                    worstAdjacentPreset = presetName;
                }
                prev = y;
            }
            previousOut[ch] = buffer.getSample(ch, blockSize - 1);
        }

        havePrevious = true;
    };

    for (int i = 0; i < warmBlocks; ++i)
        renderBlock("warmup", false);

    const auto names = processor.getFactoryPresetNames();
    for (const auto& name : names)
    {
        processor.loadFactoryPreset(name);
        renderBlock(name, true);
        for (int i = 0; i < settleBlocksPerPreset; ++i)
            renderBlock(name, false);
    }

    const bool countOk = (names.size() == static_cast<int>(FactoryPresetLibrary::kFactoryPresets.size()));
    const bool finiteOk = (invalidSamples == 0);
    const bool peakOk = maxPeak <= maxPeakAllowed;
    const bool boundaryOk = maxBoundaryJump <= maxBoundaryJumpAllowed;
    const bool adjacentOk = maxAdjacentJump <= maxAdjacentJumpAllowed;
    const bool passed = countOk && finiteOk && peakOk && boundaryOk && adjacentOk;

    juce::String result;
    result << "ActiveFactoryPresetSwitchContinuity:"
           << " presets=" << names.size() << " boundary=" << juce::String(maxBoundaryJump, 4) << "("
           << worstBoundaryPreset << ")"
           << " adjacent=" << juce::String(maxAdjacentJump, 4) << "(" << worstAdjacentPreset << ")"
           << " peak=" << juce::String(maxPeak, 4) << "(" << worstPeakPreset << ")"
           << " invalid=" << invalidSamples << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runFactoryPresetCategoryIntegrityTest(SpatialEchoAudioProcessor& processor)
{
    juce::ignoreUnused(processor);

    auto findParameterIndex = [](const char* id) -> size_t
    {
        for (size_t i = 0; i < FactoryPresetLibrary::kParameterIds.size(); ++i)
            if (std::strcmp(FactoryPresetLibrary::kParameterIds[i], id) == 0)
                return i;
        return FactoryPresetLibrary::kParameterIds.size();
    };

    const size_t mixIndex = findParameterIndex("Mix");
    if (mixIndex >= FactoryPresetLibrary::kParameterIds.size())
        return "FactoryPresetCategoryIntegrity: missing Mix index [FAIL]";

    juce::StringArray categoryIds;
    juce::StringArray usedCategoryIds;
    int invalidCategoryMetadata = 0;
    int duplicateCategoryIds = 0;
    int missingPresetCategories = 0;
    int unusedCategories = 0;
    int categoryCountMismatches = 0;
    int belowFloor = 0;
    juce::String firstIssue;

    for (const auto& category : FactoryPresetLibrary::kFactoryPresetCategories)
    {
        const bool idOk = category.id != nullptr && std::strlen(category.id) > 0;
        const bool labelOk = category.label != nullptr && std::strlen(category.label) > 0;
        const bool floorOk = std::isfinite(category.mixFloor) && category.mixFloor >= 0.0f && category.mixFloor <= 1.0f;
        if (!idOk || !labelOk || !floorOk)
        {
            ++invalidCategoryMetadata;
            if (firstIssue.isEmpty())
                firstIssue = "bad category metadata";
        }

        const juce::String id = idOk ? juce::String(category.id) : juce::String();
        if (id.isNotEmpty())
        {
            if (categoryIds.contains(id))
            {
                ++duplicateCategoryIds;
                if (firstIssue.isEmpty())
                    firstIssue = "duplicate " + id;
            }
            categoryIds.add(id);
        }
    }

    for (const auto& definition : FactoryPresetLibrary::kFactoryPresets)
    {
        const auto* category = FactoryPresetLibrary::findFactoryPresetCategory(definition.category);
        if (category == nullptr)
        {
            ++missingPresetCategories;
            if (firstIssue.isEmpty())
                firstIssue = juce::String(definition.name) + " category";
            continue;
        }

        usedCategoryIds.addIfNotAlreadyThere(category->id);
        if (definition.values[mixIndex] + 1.0e-6f < category->mixFloor)
        {
            ++belowFloor;
            if (firstIssue.isEmpty())
                firstIssue = juce::String(definition.name) + " mix";
        }
    }

    for (const auto& category : FactoryPresetLibrary::kFactoryPresetCategories)
    {
        if (category.id != nullptr && !usedCategoryIds.contains(category.id))
        {
            ++unusedCategories;
            if (firstIssue.isEmpty())
                firstIssue = juce::String(category.id) + " unused";
        }

        int presetsInCategory = 0;
        for (const auto& definition : FactoryPresetLibrary::kFactoryPresets)
            if (category.id != nullptr && definition.category != nullptr &&
                std::strcmp(definition.category, category.id) == 0)
                ++presetsInCategory;

        struct ExpectedCategoryCount
        {
            const char* id;
            int count;
        };
        constexpr std::array<ExpectedCategoryCount, 10> expectedCounts{{
            {"spatial_anchors", 5},
            {"mix_utilities", 6},
            {"throws", 8},
            {"orbits", 5},
            {"cloud_beds", 5},
            {"dub_feedback", 3},
            {"glitch_patterns", 7},
            {"transitions_impacts", 5},
            {"psycho_lab", 10},
            {"motion_lab", 8},
        }};
        int expectedCount = -1;
        for (const auto& expected : expectedCounts)
            if (category.id != nullptr && std::strcmp(category.id, expected.id) == 0)
                expectedCount = expected.count;
        if (presetsInCategory != expectedCount)
        {
            ++categoryCountMismatches;
            if (firstIssue.isEmpty())
                firstIssue = juce::String(category.id) + " count=" + juce::String(presetsInCategory);
        }
    }

    const bool passed = invalidCategoryMetadata == 0 && duplicateCategoryIds == 0 && missingPresetCategories == 0 &&
                        unusedCategories == 0 && categoryCountMismatches == 0 && belowFloor == 0 &&
                        FactoryPresetLibrary::kFactoryPresetCategories.size() == 10;

    juce::String result;
    result << "FactoryPresetCategoryIntegrity:"
           << " categories=" << FactoryPresetLibrary::kFactoryPresetCategories.size()
           << " presets=" << FactoryPresetLibrary::kFactoryPresets.size() << " invalid=" << invalidCategoryMetadata
           << " duplicate=" << duplicateCategoryIds << " missing=" << missingPresetCategories
           << " unused=" << unusedCategories << " countMismatch=" << categoryCountMismatches
           << " belowFloor=" << belowFloor;
    if (firstIssue.isNotEmpty())
        result << " first=" << firstIssue;
    result << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runFactoryPresetDescriptionCoverageTest(SpatialEchoAudioProcessor& processor)
{
    juce::ignoreUnused(processor);

    // Every shipped factory preset has description/search copy and one valid
    // UI browse assignment. The shorter authored copy still honours its budget
    // even though the browser now wraps full descriptions beneath each name.
    int missing = 0;
    int overlong = 0;
    int stale = 0;
    int missingSearchTerms = 0;
    int missingBrowseAssignments = 0;
    int staleBrowseAssignments = 0;
    int duplicateBrowseAssignments = 0;
    int invalidBrowseGroups = 0;
    int showcaseCount = 0;
    int showcaseMismatches = 0;
    juce::String firstIssue;

    constexpr std::array<const char*, 12> expectedShowcaseNames{{
        "Point Orbit",
        "Beat Orbit",
        "Front Swing",
        "Near Ear Pass",
        "Side Relay",
        "Orbit Replies",
        "Approach Retreat",
        "Near Far Relay",
        "Rear Window",
        "Front Back Relay",
        "Height Relay",
        "Sky Figure Eight",
    }};

    for (const auto& definition : FactoryPresetLibrary::kFactoryPresets)
    {
        const char* description = FactoryPresetDescriptions::find(definition.name);
        if (std::strlen(FactoryPresetDescriptions::findSearchTerms(definition.name)) == 0)
            ++missingSearchTerms;
        if (description == nullptr || std::strlen(description) == 0)
        {
            ++missing;
            if (firstIssue.isEmpty())
                firstIssue = juce::String(definition.name) + " missing";
        }
        else if (std::strlen(description) > FactoryPresetDescriptions::kMaxDescriptionLength)
        {
            ++overlong;
            if (firstIssue.isEmpty())
                firstIssue = juce::String(definition.name) + " overlong";
        }

        if (FactoryPresetBrowseGroups::find(definition.name) == nullptr)
        {
            ++missingBrowseAssignments;
            if (firstIssue.isEmpty())
                firstIssue = juce::String(definition.name) + " browse assignment missing";
        }

        bool expectedShowcase = false;
        for (const auto* name : expectedShowcaseNames)
            if (std::strcmp(name, definition.name) == 0)
                expectedShowcase = true;

        const bool searchableShowcase =
            juce::String(FactoryPresetDescriptions::findSearchTerms(definition.name)).containsWholeWord("showcase");
        if (searchableShowcase)
            ++showcaseCount;
        if (searchableShowcase != expectedShowcase ||
            (expectedShowcase && FactoryPresetBrowseGroups::groupIndexFor(definition.name) !=
                                     static_cast<int>(FactoryPresetBrowseGroups::Group::spatialAndMotion)))
        {
            ++showcaseMismatches;
            if (firstIssue.isEmpty())
                firstIssue = juce::String(definition.name) + " showcase mismatch";
        }
    }

    for (size_t i = 0; i < FactoryPresetBrowseGroups::kAssignments.size(); ++i)
    {
        const auto& entry = FactoryPresetBrowseGroups::kAssignments[i];
        const int groupIndex = static_cast<int>(entry.group);
        if (groupIndex < 0 || groupIndex >= static_cast<int>(FactoryPresetBrowseGroups::kGroupLabels.size()) ||
            FactoryPresetBrowseGroups::groupIndexFor(entry.name) != groupIndex)
        {
            ++invalidBrowseGroups;
            if (firstIssue.isEmpty())
                firstIssue = juce::String(entry.name) + " browse group invalid";
        }

        for (size_t j = 0; j < i; ++j)
            if (std::strcmp(entry.name, FactoryPresetBrowseGroups::kAssignments[j].name) == 0)
            {
                ++duplicateBrowseAssignments;
                if (firstIssue.isEmpty())
                    firstIssue = juce::String(entry.name) + " browse assignment duplicated";
            }

        bool found = false;
        for (const auto& definition : FactoryPresetLibrary::kFactoryPresets)
            if (std::strcmp(entry.name, definition.name) == 0)
                found = true;
        if (!found)
        {
            ++staleBrowseAssignments;
            if (firstIssue.isEmpty())
                firstIssue = juce::String(entry.name) + " browse assignment stale";
        }
    }

    for (const auto& entry : FactoryPresetDescriptions::kDescriptions)
    {
        bool found = false;
        for (const auto& definition : FactoryPresetLibrary::kFactoryPresets)
            if (std::strcmp(definition.name, entry.name) == 0)
            {
                found = true;
                break;
            }
        if (!found)
        {
            ++stale;
            if (firstIssue.isEmpty())
                firstIssue = juce::String(entry.name) + " stale";
        }
    }

    const bool initOk =
        std::strlen(FactoryPresetDescriptions::kInitDescription) > 0 &&
        std::strlen(FactoryPresetDescriptions::kInitDescription) <= FactoryPresetDescriptions::kMaxDescriptionLength;

    const bool passed =
        missing == 0 && overlong == 0 && stale == 0 && missingSearchTerms == 0 && initOk &&
        FactoryPresetDescriptions::kDescriptionCount == FactoryPresetLibrary::kFactoryPresets.size() &&
        FactoryPresetBrowseGroups::kGroupLabels.size() == 4 &&
        FactoryPresetBrowseGroups::kAssignments.size() == FactoryPresetLibrary::kFactoryPresets.size() &&
        missingBrowseAssignments == 0 && staleBrowseAssignments == 0 && duplicateBrowseAssignments == 0 &&
        invalidBrowseGroups == 0 && showcaseCount == static_cast<int>(expectedShowcaseNames.size()) &&
        showcaseMismatches == 0 && FactoryPresetBrowseGroups::find(nullptr) == nullptr &&
        FactoryPresetBrowseGroups::groupIndexFor("not a factory preset") == -1;

    juce::String result;
    result << "FactoryPresetDescriptionCoverage:"
           << " presets=" << (int)FactoryPresetLibrary::kFactoryPresets.size()
           << " descriptions=" << (int)FactoryPresetDescriptions::kDescriptionCount << " missing=" << missing
           << " overlong=" << overlong << " stale=" << stale << " missingSearchTerms=" << missingSearchTerms
           << " browseAssignments=" << (int)FactoryPresetBrowseGroups::kAssignments.size()
           << " missingBrowse=" << missingBrowseAssignments << " staleBrowse=" << staleBrowseAssignments
           << " duplicateBrowse=" << duplicateBrowseAssignments << " invalidBrowseGroups=" << invalidBrowseGroups
           << " showcase=" << showcaseCount << " showcaseMismatches=" << showcaseMismatches
           << " initOk=" << (initOk ? 1 : 0);
    if (firstIssue.isNotEmpty())
        result << " first=" << firstIssue;
    result << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runFactoryPresetLoadTest(SpatialEchoAudioProcessor& processor)
{
    auto& vts = processor.getValueTreeState();
    auto readBool = [&](const char* id) -> bool
    {
        if (auto* p = vts.getRawParameterValue(id))
            return p->load() > 0.5f;
        return false;
    };
    auto setPlain = [&](const char* id, float plainValue)
    {
        if (auto* p = vts.getParameter(id))
            p->setValueNotifyingHost(p->convertTo0to1(plainValue));
    };

    const auto names = processor.getFactoryPresetNames();
    constexpr std::array expectedNames{
        "Halo Orbit",           "SubSafe Orbit",      "Triplet Air Anchor",   "Vocal Halo Doubler",
        "Slap Tighten",         "Dotted 8 Grid",      "Clean PingPong Throw", "Dotted 8 Lead Throw",
        "Wide Filter Throw",    "Dust Carousel",      "Wander Morph",         "PingPong Tunnel",
        "Tape Drift Atmos",     "Dark Spring Room",   "Whole-Note Ambient",   "Dub Spiral Feedback",
        "Echo Into Spring",     "Infinite Pad Ride",  "Golden Gallop Drive",  "Bright Glitch Echo",
        "Broken Loop Dropout",  "Helix Surge",        "Hard Lock Drop Hits",  "Rush 16 Fill",
        "Externalizer Wash",    "Crossover Clarity",  "Elevation Cue",        "Envelopment Hall",
        "Motion Tracker Clean", "Ping-Pong Ricochet", "Cloud Packets",        "Forest Afterglow",
        "Crosscurrent",         "Offbeat Weave",      "Velvet Answer",        "Pond Round",
        "Distant Lantern",      "Spring Aftershock",  "Bass Reply",           "Hat Ghost Triplets",
        "Quarter Adlib",        "Halfstep Shadow",    "Riddim Pocket Room",   "Lead Gap Cloud",
        "Canon Windows",        "Bell Counterphrase", "Concrete Chatter",     "Hidden Rhythm",
        "Static Sweep",         "Spatial Pull",       "Point Orbit",          "Beat Orbit",
        "Front Swing",          "Near Ear Pass",      "Side Relay",           "Orbit Replies",
        "Approach Retreat",     "Near Far Relay",     "Rear Window",          "Front Back Relay",
        "Height Relay",         "Sky Figure Eight",
    };

    const bool countOk = names.size() == static_cast<int>(expectedNames.size()) &&
                         FactoryPresetLibrary::kFactoryPresets.size() == expectedNames.size() &&
                         FactoryPresetLibrary::kNumFactoryPresets == expectedNames.size();

    bool orderOk = countOk;
    int overlongNames = 0;
    for (size_t i = 0; i < FactoryPresetLibrary::kFactoryPresets.size(); ++i)
    {
        const auto& definition = FactoryPresetLibrary::kFactoryPresets[i];
        if (i >= expectedNames.size() || std::strcmp(definition.name, expectedNames[i]) != 0 ||
            names[static_cast<int>(i)] != expectedNames[i])
            orderOk = false;
        if (std::strlen(definition.name) > 20)
            ++overlongNames;
    }

    // This is intentionally duplicated test authority. If production inserts,
    // removes, or reorders an ID without migrating every 64-value preset row,
    // this test must fail instead of accepting the production array as truth.
    constexpr std::array<const char*, 64> expectedParameterIds{{
        "PreampPreset",
        "InputLevel",
        "Bass",
        "Treble",
        "DelaySetting",
        "RepeatRate",
        "TempoSync",
        "Head1NoteDivision",
        "Head2NoteDivision",
        "Head3NoteDivision",
        "HeadRatioPreset",
        "Intensity",
        "EchoSend",
        "Power",
        "PingPong",
        "ReverbSize",
        "ReverbDecay",
        "ReverbTone",
        "ReverbMod",
        "ReverbCharacter",
        "ReverbPreDelay",
        "ReverbWidth",
        "ReverbRouting",
        "ReverbEngine",
        "Mix",
        "EchoLevel",
        "ReverbLevel",
        "OutputGain",
        "MixLock",
        "OutputProtection",
        "FeedbackCompensation",
        "TapeDrive",
        "TapeFlavor",
        "SpatialEnabled",
        "SpatialAzimuth",
        "SpatialElevation",
        "SpatialDistance",
        "SpatialLockAzimuth",
        "SpatialLockDistance",
        "SpatialLockElevation",
        "SpatialMix",
        "SpatialCrossover",
        "SpatialQuality",
        "SpatialSourceWidth",
        "SpatialRouting",
        "HeadPositionMode",
        "Head1Azimuth",
        "Head1Elevation",
        "Head1Distance",
        "Head2Azimuth",
        "Head2Elevation",
        "Head2Distance",
        "Head3Azimuth",
        "Head3Elevation",
        "Head3Distance",
        "SpatialPattern",
        "SpatialRate",
        "SpatialSync",
        "SpatialDivision",
        "SpatialDepth",
        "SpatialRestart",
        "SpatialChoreography",
        "SpatialSpread",
        "ReverbDuckAmount",
    }};

    bool parameterSchemaOk = FactoryPresetLibrary::kParameterIds.size() == expectedParameterIds.size();
    for (size_t i = 0; i < expectedParameterIds.size(); ++i)
    {
        if (i >= FactoryPresetLibrary::kParameterIds.size() ||
            std::strcmp(FactoryPresetLibrary::kParameterIds[i], expectedParameterIds[i]) != 0)
            parameterSchemaOk = false;
    }

    const bool existsApiOk =
        processor.factoryPresetExists("Helix Surge") && !processor.factoryPresetExists("NotARealFactoryPreset");

    int missingParameters = 0;
    int valueMismatches = 0;
    juce::String firstValueIssue;
    for (const auto& definition : FactoryPresetLibrary::kFactoryPresets)
    {
        processor.loadFactoryPreset(definition.name);
        for (size_t i = 0; i < FactoryPresetLibrary::kParameterIds.size(); ++i)
        {
            const auto* parameterId = FactoryPresetLibrary::kParameterIds[i];
            const auto* raw = vts.getRawParameterValue(parameterId);
            if (raw == nullptr)
            {
                ++missingParameters;
                if (firstValueIssue.isEmpty())
                    firstValueIssue = juce::String(definition.name) + ":missing:" + parameterId;
                continue;
            }

            const float actual = raw->load();
            const float expected = definition.values[i];
            if (!std::isfinite(actual) || std::abs(actual - expected) > 1.0e-3f)
            {
                ++valueMismatches;
                if (firstValueIssue.isEmpty())
                    firstValueIssue = juce::String(definition.name) + ":" + parameterId + "=" +
                                      juce::String(actual, 4) + "/" + juce::String(expected, 4);
            }
        }
    }

    // Tape Noise is deliberately absent from the 64-value factory schema.
    // Loading a factory row must restore its Init default rather than carrying
    // a prior user override into the factory sound.
    setPlain("TapeNoiseEnabled", 0.0f);
    processor.loadFactoryPreset("Halo Orbit");
    const bool tapeNoiseResetToDefault = readBool("TapeNoiseEnabled");

    const auto* mixValue = vts.getRawParameterValue("Mix");
    const float mixBeforeInvalidLoad = mixValue != nullptr ? mixValue->load() : std::numeric_limits<float>::quiet_NaN();
    processor.loadFactoryPreset("DOES_NOT_EXIST");
    const bool invalidNoop = mixValue != nullptr && std::abs(mixValue->load() - mixBeforeInvalidLoad) <= 1.0e-6f;

    const bool passed = countOk && orderOk && overlongNames == 0 && parameterSchemaOk && existsApiOk &&
                        missingParameters == 0 && valueMismatches == 0 && invalidNoop && tapeNoiseResetToDefault;

    juce::String result;
    result << "FactoryPresetLoad: names=" << names.size() << " order=" << (orderOk ? "ok" : "bad")
           << " overlong=" << overlongNames << " schema64=" << (parameterSchemaOk ? "ok" : "bad")
           << " existsApi=" << (existsApiOk ? "ok" : "bad") << " missingParams=" << missingParameters
           << " valueMismatch=" << valueMismatches << " tapeNoiseDefault=" << (tapeNoiseResetToDefault ? "yes" : "no")
           << " invalidNoop=" << (invalidNoop ? "yes" : "no");
    if (firstValueIssue.isNotEmpty())
        result << " first=" << firstValueIssue;
    result << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runDefaultPresetAutoLoadTest(SpatialEchoAudioProcessor& processor)
{
    // Contract: a wrapper-hosted instance auto-loads the first factory preset
    // on construction. Guards:
    //  1. direct construction (wrapperType_Undefined - headless tests and
    //     offline render tools) keeps the INIT parameter defaults;
    //  2. host session restore (setStateInformation after construction)
    //     overrides the auto-loaded preset.
    juce::ignoreUnused(processor);

    auto readPlain = [](SpatialEchoAudioProcessor& p, const char* id) -> float
    {
        if (auto* raw = p.getValueTreeState().getRawParameterValue(id))
            return raw->load();
        return std::numeric_limits<float>::quiet_NaN();
    };
    auto defaultPlain = [](SpatialEchoAudioProcessor& p, const char* id) -> float
    {
        if (auto* param = p.getValueTreeState().getParameter(id))
            return param->convertFrom0to1(param->getDefaultValue());
        return std::numeric_limits<float>::quiet_NaN();
    };
    auto presetValue = [](const char* id) -> float
    {
        for (size_t i = 0; i < FactoryPresetLibrary::kParameterIds.size(); ++i)
            if (std::strcmp(FactoryPresetLibrary::kParameterIds[i], id) == 0)
                return FactoryPresetLibrary::kFactoryPresets.front().values[i];
        return std::numeric_limits<float>::quiet_NaN();
    };

    // Distinguishing parameters: the first factory preset is voiced away
    // from the INIT defaults on both (INIT: Mix 0.37, DelaySetting 0).
    const float firstMix = presetValue("Mix");
    const float firstMode = presetValue("DelaySetting");
    const bool distinguishable = std::isfinite(firstMix) && std::isfinite(firstMode);

    // 1) Direct construction: INIT defaults preserved (no auto-load).
    float directMix = 0.0f;
    float directMode = 0.0f;
    float initMix = 0.0f;
    float initMode = 0.0f;
    {
        SpatialEchoAudioProcessor direct;
        directMix = readPlain(direct, "Mix");
        directMode = readPlain(direct, "DelaySetting");
        initMix = defaultPlain(direct, "Mix");
        initMode = defaultPlain(direct, "DelaySetting");
    }
    const bool headlessGuardOk = std::abs(directMix - initMix) <= 1.0e-3f && std::abs(directMode - initMode) <= 1.0e-3f;
    const bool defaultsDiffer = std::abs(firstMix - initMix) > 1.0e-3f && std::abs(firstMode - initMode) > 1.0e-3f;

    // Exception-safe wrapper-type override: the thread-local set by
    // setTypeOfNextNewPlugin persists, so a throwing construction must not
    // leave later direct constructions typed as VST3.
    struct ScopedWrapperType
    {
        explicit ScopedWrapperType(juce::AudioProcessor::WrapperType type)
        {
            juce::AudioProcessor::setTypeOfNextNewPlugin(type);
        }
        ~ScopedWrapperType()
        {
            juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
        }
    };

    // 2) Wrapper-hosted construction: first factory preset auto-loads, the
    //    preset identity is stamped, and the startup load is not undoable.
    float hostedMix = 0.0f;
    float hostedMode = 0.0f;
    juce::String hostedPresetName;
    int hostedPresetType = -1;
    bool hostedUndoEmpty = false;
    {
        ScopedWrapperType scopedType(juce::AudioProcessor::wrapperType_VST3);
        SpatialEchoAudioProcessor hosted;
        hostedMix = readPlain(hosted, "Mix");
        hostedMode = readPlain(hosted, "DelaySetting");
        const auto& state = hosted.getValueTreeState().state;
        hostedPresetName = state.getProperty("activePresetName", juce::String()).toString();
        hostedPresetType = static_cast<int>(state.getProperty("activePresetType", 0));
        hostedUndoEmpty = !hosted.getUndoManager().canUndo();
    }
    const bool autoLoadOk = std::abs(hostedMix - firstMix) <= 1.0e-3f && std::abs(hostedMode - firstMode) <= 1.0e-3f;
    const bool identityOk = hostedPresetName == FactoryPresetLibrary::kFactoryPresets.front().name &&
                            hostedPresetType == 1;  // PresetSelectionType::Factory

    // 3) Session restore wins over the auto-load.
    constexpr float sentinelMix = 0.91f;
    juce::MemoryBlock sentinelState;
    {
        SpatialEchoAudioProcessor donor;
        auto& donorVts = donor.getValueTreeState();
        if (auto* p = donorVts.getParameter("Mix"))
            p->setValueNotifyingHost(p->convertTo0to1(sentinelMix));
        donor.getStateInformation(sentinelState);
    }
    float restoredMix = 0.0f;
    {
        ScopedWrapperType scopedType(juce::AudioProcessor::wrapperType_VST3);
        SpatialEchoAudioProcessor hosted;
        hosted.setStateInformation(sentinelState.getData(), static_cast<int>(sentinelState.getSize()));
        restoredMix = readPlain(hosted, "Mix");
    }
    const bool restoreWinsOk = std::abs(restoredMix - sentinelMix) <= 1.0e-3f;

    const bool passed = distinguishable && defaultsDiffer && headlessGuardOk && autoLoadOk && identityOk &&
                        hostedUndoEmpty && restoreWinsOk;

    juce::String result;
    result << "DefaultPresetAutoLoad:"
           << " first=" << FactoryPresetLibrary::kFactoryPresets.front().name
           << " direct{mix=" << juce::String(directMix, 3) << " mode=" << juce::String(directMode, 1) << "}"
           << " hosted{mix=" << juce::String(hostedMix, 3) << " mode=" << juce::String(hostedMode, 1) << "}"
           << " restoredMix=" << juce::String(restoredMix, 3) << " headlessGuard=" << (headlessGuardOk ? "ok" : "bad")
           << " autoLoad=" << (autoLoadOk ? "ok" : "bad") << " identity="
           << (identityOk ? "ok" : ("bad(" + hostedPresetName + "/" + juce::String(hostedPresetType) + ")"))
           << " undoEmpty=" << (hostedUndoEmpty ? "yes" : "no") << " restoreWins=" << (restoreWinsOk ? "ok" : "bad")
           << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runPresetLoadUndoRecoveryTest(SpatialEchoAudioProcessor& processor)
{
    // Contract: browsing presets must never destroy unsaved edits
    // irrecoverably. The editor begins an undo transaction before each load
    // (PluginEditor::loadPresetSelection), so a single undo after a preset
    // load must restore the user's pre-load edit.
    auto& vts = processor.getValueTreeState();
    auto& undo = processor.getUndoManager();

    auto setPlain = [&](const char* id, float plain)
    {
        if (auto* p = vts.getParameter(id))
            p->setValueNotifyingHost(p->convertTo0to1(plain));
    };
    auto readFloat = [&](const char* id) -> float
    {
        if (auto* p = vts.getRawParameterValue(id))
            return p->load();
        return std::numeric_limits<float>::quiet_NaN();
    };

    // The APVTS flushes parameter values to its ValueTree (recording them in
    // the UndoManager) from a message-thread timer; copyState() forces that
    // flush synchronously, keeping this test deterministic.
    auto flushToTree = [&]() { vts.copyState(); };
    auto treeValueMatches = [&](const char* id, float plainExpected) -> bool
    {
        const auto child = vts.state.getChildWithProperty("id", juce::String(id));
        if (!child.isValid())
            return false;
        return std::abs(static_cast<float>(child.getProperty("value")) - plainExpected) <= 1.0e-3f;
    };

    auto factoryValue = [](const char* presetName, const char* parameterId) -> float
    {
        size_t parameterIndex = FactoryPresetLibrary::kParameterIds.size();
        for (size_t i = 0; i < FactoryPresetLibrary::kParameterIds.size(); ++i)
            if (std::strcmp(FactoryPresetLibrary::kParameterIds[i], parameterId) == 0)
            {
                parameterIndex = i;
                break;
            }

        if (parameterIndex >= FactoryPresetLibrary::kParameterIds.size())
            return std::numeric_limits<float>::quiet_NaN();

        for (const auto& definition : FactoryPresetLibrary::kFactoryPresets)
            if (std::strcmp(definition.name, presetName) == 0)
                return definition.values[parameterIndex];

        return std::numeric_limits<float>::quiet_NaN();
    };

    // Deterministic baseline, then simulate a user edit on top of it.
    processor.loadFactoryPreset("Halo Orbit");
    flushToTree();
    undo.clearUndoHistory();

    constexpr float editedMix = 0.91f;
    setPlain("Mix", editedMix);
    flushToTree();
    const bool editApplied = treeValueMatches("Mix", editedMix);

    // Mirror the editor's preset-navigation path: transaction boundary + load.
    undo.beginNewTransaction("Load Preset");
    processor.loadFactoryPreset("SubSafe Orbit");
    flushToTree();
    const float loadedMix = factoryValue("SubSafe Orbit", "Mix");
    const bool loadApplied = std::isfinite(loadedMix) && treeValueMatches("Mix", loadedMix);

    const bool undoAvailable = undo.canUndo();
    const bool undone = undoAvailable && undo.undo();
    const float mixAfterUndo = readFloat("Mix");
    const bool editRecovered = std::abs(mixAfterUndo - editedMix) <= 1.0e-3f;

    const bool passed = editApplied && loadApplied && undoAvailable && undone && editRecovered;

    juce::String result;
    result << "PresetLoadUndoRecovery:"
           << " editApplied=" << (editApplied ? "yes" : "no") << " loadApplied=" << (loadApplied ? "yes" : "no")
           << " undoAvailable=" << (undoAvailable ? "yes" : "no") << " undone=" << (undone ? "yes" : "no")
           << " mixAfterUndo=" << juce::String(mixAfterUndo, 3) << " editRecovered=" << (editRecovered ? "yes" : "no")
           << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

// PresetLoadSilenceRate: INSTRUMENT with a loose gate, not a tight contract.
//
// The plugin intermittently emits audible output from EXACT SILENCE in the first
// blocks after a preset load. FactoryPresetNoisePolicy shows the same signature
// on roughly 2 of 5 runs. That test skips blocks 0-2 (warmupBlocks) before
// reading its probes, so the transient is never measured on the input side
// while its decaying tail IS measured downstream -- which is why it presents as
// "the reverb emitting from nothing" when the reverb is provably bit-zero from
// bit-zero input.
//
// The cause is measured but not yet attributed. Firing rate at a -80 dBFS
// threshold, 30 fresh processors per variant, two independent sets:
//   set 1:  free std::rand() 7/30 (worst -48.1)  |  PINNED seed 1/30 (worst -52.1)
//   set 2:  free std::rand() 4/30 (worst -45.7)  |  PINNED seed 5/30 (worst -52.1)
// The first set suggested the global std::rand() dependency (CloudSeed's
// ModulatedDelay and ModulatedAllpass seed their modulation phase from it) was
// the primary cause; the second reversed the ordering. Pooled, 11/60 vs 6/60 is
// suggestive at best and NOT significant at this sample size. Settling this
// needs several hundred repetitions per variant.
//
// Hypotheses already ruled out by measurement:
//   - the reverb emitting on its own (proven bit-zero out from bit-zero in,
//     five fresh processes, plus a bit-identical impulse checksum)
//   - uninitialised CloudSeed delay buffers (they are declared `= { 0 }`)
//
// The gate is deliberately loose. The rate is a statistic, so a tight bound would
// be flaky by construction; this catches a gross regression (the fault becoming
// the common case) without pretending to a precision it cannot have.
// Diagnostic goal: find what emits a transient in the first blocks after a
// preset load, from silent input. Input to processBlock is exact silence every
// block, so whatever this is, the plugin generates it internally.
juce::String runPresetLoadSilenceRate(SpatialEchoAudioProcessor& /*processor*/)
{
    const double fs = 48000.0;
    const int blockSize = 512;
    const int probeBlocks = 12;
    const int reps = 30;
    const char* kPreset = "Dark Spring Room";

    juce::String out;

    auto runOne = [&](bool fixedSeed) -> juce::String
    {
        // Decisive test for the leading hypothesis: CloudSeed's ModulatedDelay and
        // ModulatedAllpass seed their modulation phase from the process-global
        // std::rand(). If pinning that seed collapses the run-to-run spread, the
        // global PRNG dependency is the cause.
        if (fixedSeed)
            std::srand(0x5EED);
        SpatialEchoAudioProcessor probe;
        probe.loadFactoryPreset(kPreset);
        probe.prepareToPlay(fs, blockSize);

        juce::AudioBuffer<float> buffer(2, blockSize);
        juce::MidiBuffer midi;
        float worst = 0.0f;
        for (int bl = 0; bl < probeBlocks; ++bl)
        {
            buffer.clear();
            probe.processBlock(buffer, midi);
            double sumSq = 0.0;
            int n = 0;
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const float* d = buffer.getReadPointer(ch);
                for (int i = 0; i < blockSize; ++i)
                {
                    sumSq += (double)d[i] * (double)d[i];
                    ++n;
                }
            }
            worst = std::max(worst, (n > 0) ? (float)std::sqrt(sumSq / n) : 0.0f);
        }
        return juce::String(TestUtilities::linearToDb(worst), 1);
    };

    // Count the FIRING RATE rather than eyeballing samples: the fault is
    // intermittent, so a handful of repetitions has no power to compare variants.
    int firedFree = 0;
    float worstFree = -999.0f;
    auto rate = [&](const char* label, bool fixedSeed)
    {
        int fired = 0;
        float worst = -999.0f;
        for (int r = 0; r < reps; ++r)
        {
            const float db = runOne(fixedSeed).getFloatValue();
            if (db > -80.0f)
                ++fired;
            worst = std::max(worst, db);
        }
        out << "\n  " << label << " fired " << fired << "/" << reps
            << " above -80dBFS, worst=" << juce::String(worst, 1) << "dBFS";
        if (!fixedSeed)
        {
            firedFree = fired;
            worstFree = worst;
        }
    };
    rate("free  rand() :", false);
    rate("PINNED seed  :", true);

    // Fail only on a gross regression: the fault becoming the majority case, or
    // a level that is unambiguously audible rather than a low-level artefact.
    const bool rateOk = (firedFree <= reps / 2);
    const bool levelOk = (worstFree <= -30.0f);
    const bool passed = rateOk && levelOk;
    out << "\n  gates: rate<=" << (reps / 2) << "/" << reps << " and worst<=-30.0dBFS";
    return juce::String(passed ? "[PASS] " : "[FAIL] ") + "PresetLoadSilenceRate (instrument):" + out;
}

juce::String runFactoryPresetNoisePolicyTest(SpatialEchoAudioProcessor& processor)
{
    juce::ignoreUnused(processor);
    constexpr float fs = 48000.0f;
    constexpr int blockSize = 512;
    constexpr int blocks = 18;
    constexpr int warmupBlocks = 3;
    const float quietThreshold = TestUtilities::dbToLinear(-120.0f);
    const float audibleThreshold = TestUtilities::dbToLinear(-125.0f);

    auto findParameterIndex = [](const char* id) -> size_t
    {
        for (size_t i = 0; i < FactoryPresetLibrary::kParameterIds.size(); ++i)
            if (std::strcmp(FactoryPresetLibrary::kParameterIds[i], id) == 0)
                return i;
        return FactoryPresetLibrary::kParameterIds.size();
    };

    // Curated factory intent is pinned independently from both preset values
    // and TapeFlavors::noiseEnabled. A flavor or roster edit must not silently
    // redefine which named sounds are allowed to emit tape noise.
    constexpr std::array<const char*, 9> expectedNoisyPresetNames{{
        "Wide Filter Throw",
        "Dust Carousel",
        "Tape Drift Atmos",
        "Dub Spiral Feedback",
        "Echo Into Spring",
        "Golden Gallop Drive",
        "Broken Loop Dropout",
        "Hard Lock Drop Hits",
        "Rush 16 Fill",
    }};
    auto isExpectedNoisyPreset = [&](const char* presetName) -> bool
    {
        for (const auto* expectedName : expectedNoisyPresetNames)
            if (std::strcmp(expectedName, presetName) == 0)
                return true;
        return false;
    };

    const size_t tapeFlavorIndex = findParameterIndex("TapeFlavor");
    if (tapeFlavorIndex >= FactoryPresetLibrary::kParameterIds.size())
        return "FactoryPresetNoisePolicy: missing TapeFlavor index [FAIL]";

    auto readBool = [](juce::AudioProcessorValueTreeState& vts, const char* id) -> bool
    {
        if (auto* p = vts.getRawParameterValue(id))
            return p->load() > 0.5f;
        return false;
    };

    auto readChoice = [](juce::AudioProcessorValueTreeState& vts, const char* id) -> int
    {
        if (auto* p = vts.getRawParameterValue(id))
            return (int)std::lround(p->load());
        return 0;
    };

    struct Metrics
    {
        float rms = 0.0f;
        float reverbInputRms = 0.0f;
        float reverbReturnRms = 0.0f;
        int invalid = 0;
    };

    auto renderSilence = [&](SpatialEchoAudioProcessor& target, juce::AudioProcessorValueTreeState& vts) -> Metrics
    {
        juce::AudioBuffer<float> buffer(2, blockSize);
        juce::MidiBuffer midi;
        const int reverbRoutingMode = std::clamp(readChoice(vts, "ReverbRouting"), 0, 2);
        double sumSquares = 0.0;
        double reverbInputSumSquares = 0.0;
        double reverbReturnSumSquares = 0.0;
        int sampleCount = 0;
        int reverbProbeBlockCount = 0;
        int invalid = 0;

        for (int b = 0; b < blocks; ++b)
        {
            buffer.clear();
            target.processBlock(buffer, midi);

            if (b < warmupBlocks)
                continue;

            float reverbInputPeak = 0.0f;
            float reverbInputRms = 0.0f;
            float reverbReturnPeak = 0.0f;
            float reverbReturnRms = 0.0f;
            const bool inputProbeOk =
                target.debugGetReverbInputMetrics(reverbRoutingMode, reverbInputPeak, reverbInputRms);
            const bool returnProbeOk =
                target.debugGetReverbReturnMetrics(reverbRoutingMode, reverbReturnPeak, reverbReturnRms);
            if (inputProbeOk && returnProbeOk && std::isfinite(reverbInputRms) && std::isfinite(reverbReturnRms))
            {
                reverbInputSumSquares += static_cast<double>(reverbInputRms) * static_cast<double>(reverbInputRms);
                reverbReturnSumSquares += static_cast<double>(reverbReturnRms) * static_cast<double>(reverbReturnRms);
                ++reverbProbeBlockCount;
            }
            else
            {
                ++invalid;
            }

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const auto* data = buffer.getReadPointer(ch);
                for (int s = 0; s < blockSize; ++s)
                {
                    const float v = data[s];
                    if (!std::isfinite(v))
                    {
                        ++invalid;
                        continue;
                    }
                    sumSquares += static_cast<double>(v) * static_cast<double>(v);
                    ++sampleCount;
                }
            }
        }

        Metrics metrics;
        metrics.invalid = invalid;
        metrics.rms =
            (sampleCount > 0) ? static_cast<float>(std::sqrt(sumSquares / static_cast<double>(sampleCount))) : 0.0f;
        metrics.reverbInputRms =
            reverbProbeBlockCount > 0
                ? static_cast<float>(std::sqrt(reverbInputSumSquares / static_cast<double>(reverbProbeBlockCount)))
                : 0.0f;
        metrics.reverbReturnRms =
            reverbProbeBlockCount > 0
                ? static_cast<float>(std::sqrt(reverbReturnSumSquares / static_cast<double>(reverbProbeBlockCount)))
                : 0.0f;
        return metrics;
    };

    int expectedNoisyCount = 0;
    int actualNoisyCount = 0;
    int stateMismatches = 0;
    int renderMismatches = 0;
    int invalidSamples = 0;
    juce::String firstMismatch;

    for (const auto& definition : FactoryPresetLibrary::kFactoryPresets)
    {
        const bool expectedNoisy = isExpectedNoisyPreset(definition.name);
        if (expectedNoisy)
            ++expectedNoisyCount;

        SpatialEchoAudioProcessor presetProbe;
        auto& probeVts = presetProbe.getValueTreeState();
        presetProbe.debugEnableReverbInputProbe(true);
        presetProbe.debugEnableReverbReturnProbe(true);
        presetProbe.loadFactoryPreset(definition.name);
        presetProbe.prepareToPlay(fs, blockSize);
        presetProbe.debugResetReverbInputProbe();
        presetProbe.debugResetReverbReturnProbe();

        const auto* flavorParam = dynamic_cast<juce::AudioParameterChoice*>(probeVts.getParameter("TapeFlavor"));
        const bool stateNoisy = flavorParam != nullptr && TapeFlavors::get(flavorParam->getIndex()).noiseEnabled;
        if (stateNoisy)
            ++actualNoisyCount;
        if (stateNoisy != expectedNoisy)
        {
            ++stateMismatches;
            if (firstMismatch.isEmpty())
                firstMismatch = juce::String(definition.name) + " state";
        }

        const Metrics metrics = renderSilence(presetProbe, probeVts);
        invalidSamples += metrics.invalid;
        const bool renderOk = expectedNoisy ? (metrics.rms >= audibleThreshold) : (metrics.rms <= quietThreshold);
        if (!renderOk)
        {
            ++renderMismatches;
            if (firstMismatch.isEmpty())
            {
                firstMismatch = juce::String(definition.name) +
                                " rms=" + juce::String(TestUtilities::linearToDb(metrics.rms), 1) + "dB" +
                                " revIn=" + juce::String(TestUtilities::linearToDb(metrics.reverbInputRms), 1) + "dB" +
                                " revOut=" + juce::String(TestUtilities::linearToDb(metrics.reverbReturnRms), 1) +
                                "dB" + " flavor=" + juce::String(static_cast<int>(definition.values[tapeFlavorIndex])) +
                                " power=" + juce::String(readBool(probeVts, "Power") ? 1 : 0) +
                                " bin=" + juce::String(readBool(probeVts, "SpatialEnabled") ? 1 : 0) +
                                " motion=" + juce::String(readChoice(probeVts, "SpatialPattern") != 0 ? 1 : 0);
            }
        }

        presetProbe.debugEnableReverbInputProbe(false);
        presetProbe.debugEnableReverbReturnProbe(false);
    }

    const bool policyCoverageOk = expectedNoisyCount == static_cast<int>(expectedNoisyPresetNames.size());
    const bool exercisesBothPolicies =
        expectedNoisyCount > 0 && expectedNoisyCount < static_cast<int>(FactoryPresetLibrary::kFactoryPresets.size());
    const bool passed = policyCoverageOk && exercisesBothPolicies && actualNoisyCount == expectedNoisyCount &&
                        stateMismatches == 0 && renderMismatches == 0 && invalidSamples == 0;

    juce::String result;
    result << "FactoryPresetNoisePolicy:"
           << " noisy=" << actualNoisyCount << "/" << FactoryPresetLibrary::kFactoryPresets.size()
           << " policyCoverage=" << (policyCoverageOk ? "yes" : "no")
           << " bothPolicies=" << (exercisesBothPolicies ? "yes" : "no") << " stateMismatch=" << stateMismatches
           << " renderMismatch=" << renderMismatches << " invalid=" << invalidSamples;
    if (firstMismatch.isNotEmpty())
        result << " first=" << firstMismatch;
    result << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runTapePresetNoiseEnablePolicyTest(SpatialEchoAudioProcessor& processor)
{
    constexpr float fs = 48000.0f;
    constexpr int blockSize = 512;

    auto& vts = processor.getValueTreeState();
    auto setBool = [&](const char* id, bool value) { SpatialEchoParameterWrite::setBoolById(vts, id, value); };
    auto setChoice = [&](const char* id, int index)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter(id)))
        {
            const int maxIdx = std::max(0, p->choices.size() - 1);
            const int clamped = std::max(0, std::min(index, maxIdx));
            p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(clamped)));
        }
    };
    auto processOneBlock = [&]()
    {
        juce::AudioBuffer<float> buffer(2, blockSize);
        juce::MidiBuffer midi;
        buffer.clear();
        processor.processBlock(buffer, midi);
    };

    // Noise is flavor-owned by default. The explicit override must let a user
    // keep a noisy flavor's other tape character while muting its noise bed.
    auto renderSilenceRms = [&]() -> float
    {
        juce::AudioBuffer<float> buffer(2, blockSize);
        juce::MidiBuffer midi;
        double sumSquares = 0.0;
        int count = 0;
        for (int b = 0; b < 18; ++b)
        {
            buffer.clear();
            processor.processBlock(buffer, midi);
            if (b < 3)
                continue;
            for (int ch = 0; ch < 2; ++ch)
            {
                const auto* data = buffer.getReadPointer(ch);
                for (int s = 0; s < blockSize; ++s)
                {
                    sumSquares += static_cast<double>(data[s]) * data[s];
                    ++count;
                }
            }
        }
        return count > 0 ? static_cast<float>(std::sqrt(sumSquares / count)) : 0.0f;
    };

    const float quiet = TestUtilities::dbToLinear(-120.0f);
    const float audible = TestUtilities::dbToLinear(-125.0f);

    processor.loadDefaultPreset();
    processor.prepareToPlay(fs, blockSize);
    processOneBlock();
    const float defaultRms = renderSilenceRms();
    const bool defaultSilent = defaultRms <= quiet;

    setChoice("TapeFlavor", static_cast<int>(TapeFlavor::Cassette));
    processOneBlock();
    const float cassetteRms = renderSilenceRms();
    const bool cassetteHissAudible = cassetteRms >= audible && cassetteRms > defaultRms * 8.0f;

    setBool("TapeNoiseEnabled", false);
    // Disabling noise starts the de-click fade (one-pole, 1000-sample time
    // constant at 48 kHz) that TapeNoiseTransition requires to reach exact
    // silence within 0.5 s. Measure the settled mute, not the fade.
    for (int b = 0; b < static_cast<int>(std::ceil(0.5f * fs / blockSize)); ++b)
        processOneBlock();
    const float mutedRms = renderSilenceRms();
    const bool overrideMutesNoise = mutedRms <= quiet;

    setBool("TapeNoiseEnabled", true);
    setChoice("TapeFlavor", static_cast<int>(TapeFlavor::Warm));
    processOneBlock();
    const float warmRms = renderSilenceRms();
    const bool warmSilentAgain = warmRms <= quiet;

    const bool passed = defaultSilent && cassetteHissAudible && overrideMutesNoise && warmSilentAgain;

    juce::String result;
    result << "TapePresetNoiseEnablePolicy:"
           << " default=" << juce::String(TestUtilities::linearToDb(defaultRms), 1) << "dB"
           << " cassette=" << juce::String(TestUtilities::linearToDb(cassetteRms), 1) << "dB"
           << " muted=" << juce::String(TestUtilities::linearToDb(mutedRms), 1) << "dB"
           << " warmAgain=" << juce::String(TestUtilities::linearToDb(warmRms), 1) << "dB"
           << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runStartupDefaultStateConsistencyTest(SpatialEchoAudioProcessor& processor)
{
    constexpr float fs = 48000.0f;
    constexpr int blockSize = 512;
    constexpr float eps = 1.0e-5f;
    processor.prepareToPlay(fs, blockSize);

    auto& vts = processor.getValueTreeState();
    auto readParam = [&](const char* id) -> float
    {
        if (auto* p = vts.getRawParameterValue(id))
            return p->load();
        return std::numeric_limits<float>::quiet_NaN();
    };
    auto readChoice = [&](const char* id) -> int
    {
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter(id)))
            return p->getIndex();
        return -1;
    };

    // TapeDrive + TapeFlavor are the whole tape surface. Processing must not
    // drift them: the flavor system makes no hidden preset writes.
    const float driveBefore = readParam("TapeDrive");
    const int flavorBefore = readChoice("TapeFlavor");

    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    for (int i = 0; i < 8; ++i)
    {
        buffer.clear();
        processor.processBlock(buffer, midi);
    }

    const float driveAfter = readParam("TapeDrive");
    const int flavorAfter = readChoice("TapeFlavor");

    const bool tapeControlsStable = std::abs(driveAfter - driveBefore) <= eps;
    const bool flavorStable = (flavorAfter == flavorBefore);
    const bool passed = tapeControlsStable && flavorStable;

    juce::String result;
    result << "StartupDefaultStateConsistency:"
           << " flavor=" << flavorBefore << "->" << flavorAfter << " drive=" << juce::String(driveBefore, 3) << "->"
           << juce::String(driveAfter, 3) << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runStartupDefaultNoiseFloorTest(SpatialEchoAudioProcessor& processor)
{
    constexpr double fs = 48000.0;
    constexpr int blockSize = 512;
    constexpr int totalBlocks = 220;
    constexpr int warmupBlocks = 40;
    constexpr float compressorMakeupDb = 24.0f;

    struct Metrics
    {
        float rms = 0.0f;
        float peak = 0.0f;
        int invalid = 0;
    };

    auto ampToDb = [](float amp) -> float { return 20.0f * std::log10(std::max(amp, 1.0e-9f)); };

    auto renderSilence = [&]() -> Metrics
    {
        juce::AudioBuffer<float> buffer(2, blockSize);
        juce::MidiBuffer midi;

        double sumSquares = 0.0;
        int sampleCount = 0;
        float peak = 0.0f;
        int invalid = 0;

        for (int b = 0; b < totalBlocks; ++b)
        {
            buffer.clear();
            processor.processBlock(buffer, midi);

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const auto* data = buffer.getReadPointer(ch);
                for (int s = 0; s < blockSize; ++s)
                {
                    const float v = data[s];
                    if (!std::isfinite(v))
                        ++invalid;
                    if (b < warmupBlocks)
                        continue;

                    sumSquares += static_cast<double>(v) * static_cast<double>(v);
                    peak = std::max(peak, std::abs(v));
                    ++sampleCount;
                }
            }
        }

        Metrics metrics;
        metrics.rms =
            (sampleCount > 0) ? static_cast<float>(std::sqrt(sumSquares / static_cast<double>(sampleCount))) : 0.0f;
        metrics.peak = peak;
        metrics.invalid = invalid;
        return metrics;
    };

    auto& vts = processor.getValueTreeState();
    auto readParam = [&](const char* id) -> float
    {
        if (auto* p = vts.getRawParameterValue(id))
            return p->load();
        return std::numeric_limits<float>::quiet_NaN();
    };

    processor.loadDefaultPreset();
    processor.prepareToPlay(fs, blockSize);
    // Current schema: default flavor is WARM (noise off). The hiss bed is
    // reached by selecting a noisy flavor instead of a NoiseEnabled toggle.
    const auto* flavorParam = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter("TapeFlavor"));
    const float defaultNoiseEnabled =
        (flavorParam != nullptr && TapeFlavors::get(flavorParam->getIndex()).noiseEnabled) ? 1.0f : 0.0f;
    const float defaultNoiseAmount = readParam("TapeDrive");  // reported for context only
    const Metrics defaultMetrics = renderSilence();
    const float makeupGain = juce::Decibels::decibelsToGain(compressorMakeupDb);
    const float defaultPostMakeupDb = ampToDb(defaultMetrics.rms * makeupGain);

    processor.loadDefaultPreset();
    if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter("TapeFlavor")))
        p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(static_cast<int>(TapeFlavor::Cassette))));
    processor.prepareToPlay(fs, blockSize);
    const Metrics enabledMetrics = renderSilence();
    const float enabledDb = ampToDb(enabledMetrics.rms);

    const bool defaultToggleOff = defaultNoiseEnabled <= 0.5f;
    const bool defaultFloorMuted = defaultMetrics.rms <= TestUtilities::dbToLinear(-120.0f);
    const bool compressorProbeOk = defaultPostMakeupDb <= -96.0f;
    const bool explicitNoiseAvailable =
        enabledMetrics.rms >= TestUtilities::dbToLinear(-125.0f) && enabledMetrics.rms > defaultMetrics.rms * 8.0f;
    const bool passed = (defaultMetrics.invalid == 0) && (enabledMetrics.invalid == 0) && defaultToggleOff &&
                        defaultFloorMuted && compressorProbeOk && explicitNoiseAvailable;

    juce::String result;
    result << "StartupDefaultNoiseFloor:"
           << " on=" << juce::String(defaultNoiseEnabled, 1) << " drive=" << juce::String(defaultNoiseAmount, 3)
           << " rms=" << juce::String(ampToDb(defaultMetrics.rms), 1) << "dB"
           << " peak=" << juce::String(ampToDb(defaultMetrics.peak), 1) << "dB"
           << " makeup24=" << juce::String(defaultPostMakeupDb, 1) << "dB"
           << " enabled=" << juce::String(enabledDb, 1) << "dB"
           << " invalid=" << (defaultMetrics.invalid + enabledMetrics.invalid) << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runInitDefaultParameterContractTest(SpatialEchoAudioProcessor& processor)
{
    struct ExpectedParam
    {
        const char* id;
        float value;
        float tolerance;
    };

    constexpr float kTol = 1.0e-4f;
    // Size must equal the initializer row count exactly: value-initialized
    // trailing entries carry a null id and crash the StringRef lookup.
    const std::array<ExpectedParam, 68> expected = {{
        {"PreampPreset", 0.0f, kTol},
        {"InputLevel", 0.5f, kTol},
        {"Bass", 0.5f, kTol},
        {"Treble", 0.5f, kTol},

        {"DelaySetting", 0.0f, kTol},
        {"RepeatRate", 0.5f, kTol},
        {"TempoSync", 0.0f, kTol},
        {"Head1NoteDivision", 2.0f, kTol},
        {"Head2NoteDivision", 3.0f, kTol},
        {"Head3NoteDivision", 4.0f, kTol},
        {"HeadRatioPreset", 0.0f, kTol},
        {"Intensity", 0.4f, kTol},
        {"EchoSend", 1.0f, kTol},
        {"Power", 1.0f, kTol},
        {"AutoPanic", 0.0f, kTol},
        {"PingPong", 0.0f, kTol},

        {"ReverbEngine", 0.0f, kTol},
        {"ReverbSize", 0.7085f, kTol},
        {"ReverbDecay", 0.6802f, kTol},
        {"ReverbTone", 0.5f, kTol},
        {"ReverbMod", 0.08f, kTol},
        {"ReverbCharacter", 0.5f, kTol},
        {"ReverbPreDelay", 0.0f, kTol},
        {"ReverbWidth", 0.4f, kTol},
        {"ReverbDuckAmount", 0.0f, kTol},
        {"ReverbRouting", 0.0f, kTol},

        {"Mix", 0.37f, kTol},
        {"EchoLevel", 0.0f, kTol},
        {"ReverbLevel", 0.0f, kTol},
        {"OutputGain", 0.5f, kTol},
        {"MixLock", 0.0f, kTol},
        {"OutputProtection", 1.0f, kTol},
        {"FeedbackCompensation", 1.0f, kTol},

        {"TapeDrive", 0.45f, kTol},
        {"TapeFlavor", 1.0f, kTol},  // WARM
        {"TapeNoiseEnabled", 1.0f, kTol},

        {"SpatialEnabled", 0.0f, kTol},
        {"SpatialAzimuth", 0.0f, kTol},
        {"SpatialElevation", 0.0f, kTol},
        {"SpatialDistance", 1.0f, kTol},
        {"SpatialLockAzimuth", 0.0f, kTol},
        {"SpatialLockDistance", 0.0f, kTol},
        {"SpatialLockElevation", 0.0f, kTol},
        {"SpatialMix", 100.0f, kTol},
        {"SpatialCrossover", 150.0f, kTol},
        {"SpatialQuality", 2.0f, kTol},
        {"SpatialSourceWidth", 0.5f, kTol},
        {"SpatialRouting", 0.0f, kTol},

        {"HeadPositionMode", 0.0f, kTol},
        {"Head1Azimuth", -60.0f, kTol},
        {"Head1Elevation", 0.0f, kTol},
        {"Head1Distance", 1.0f, kTol},
        {"Head2Azimuth", 60.0f, kTol},
        {"Head2Elevation", 0.0f, kTol},
        {"Head2Distance", 1.0f, kTol},
        {"Head3Azimuth", 180.0f, kTol},
        {"Head3Elevation", 0.0f, kTol},
        {"Head3Distance", 1.2f, kTol},

        {"SpatialPattern", 0.0f, kTol},
        {"SpatialRate", 0.35f, kTol},
        {"SpatialSync", 0.0f, kTol},
        {"SpatialDivision", 3.0f, kTol},
        {"SpatialDepth", 45.0f, kTol},
        {"SpatialRestart", 0.0f, kTol},
        {"SpatialChoreography", 0.0f, kTol},
        {"SpatialSpread", 0.0f, kTol},
        {"SpatialLongDivision", 0.0f, kTol},
        {"SpatialLongVariantDivision", 0.0f, kTol},
    }};

    auto& vts = processor.getValueTreeState();
    const auto* tapeNoiseParameter = vts.getParameter("TapeNoiseEnabled");
    const bool tapeNoiseVersionHintOk = tapeNoiseParameter != nullptr && tapeNoiseParameter->getVersionHint() == 4;
    float maxError = 0.0f;
    juce::String firstMismatch;

    auto checkParam = [&](const ExpectedParam& entry, const char* phase) -> int
    {
        const auto* raw = vts.getRawParameterValue(entry.id);
        if (raw == nullptr)
        {
            if (firstMismatch.isEmpty())
                firstMismatch = juce::String(phase) + ":missing:" + entry.id;
            return 1;
        }

        const float actual = raw->load();
        const float err = std::abs(actual - entry.value);
        maxError = std::max(maxError, err);
        if (err > entry.tolerance)
        {
            if (firstMismatch.isEmpty())
                firstMismatch = juce::String(phase) + ":" + entry.id + " exp=" + juce::String(entry.value, 5) +
                                " got=" + juce::String(actual, 5);
            return 1;
        }
        return 0;
    };

    auto validateAll = [&](const char* phase) -> int
    {
        int mismatches = 0;
        for (const auto& entry : expected)
            mismatches += checkParam(entry, phase);
        return mismatches;
    };

    auto mutateParam = [&](const ExpectedParam& entry) -> int
    {
        auto* param = vts.getParameter(entry.id);
        auto* raw = vts.getRawParameterValue(entry.id);
        if (param == nullptr || raw == nullptr)
            return 0;

        const float before = raw->load();

        if (auto* boolParam = dynamic_cast<juce::AudioParameterBool*>(param))
        {
            const bool target = before <= 0.5f;
            boolParam->setValueNotifyingHost(target ? 1.0f : 0.0f);
        }
        else if (auto* choiceParam = dynamic_cast<juce::AudioParameterChoice*>(param))
        {
            const int maxIdx = std::max(0, choiceParam->choices.size() - 1);
            int current = juce::jlimit(0, maxIdx, static_cast<int>(std::lround(before)));
            int next = current;
            if (maxIdx > 0)
                next = (current < maxIdx) ? (current + 1) : (current - 1);
            const float norm = (maxIdx > 0) ? static_cast<float>(next) / static_cast<float>(maxIdx) : 0.0f;
            choiceParam->setValueNotifyingHost(norm);
        }
        else if (auto* floatParam = dynamic_cast<juce::AudioParameterFloat*>(param))
        {
            const float minV = floatParam->range.start;
            const float maxV = floatParam->range.end;
            float candidate = (std::abs(before - minV) > std::abs(before - maxV)) ? minV : maxV;
            if (std::abs(candidate - before) <= entry.tolerance)
                candidate = juce::jlimit(minV, maxV, minV + (maxV - minV) * 0.37f);
            if (std::abs(candidate - before) <= entry.tolerance)
                candidate = juce::jlimit(minV, maxV, minV + (maxV - minV) * 0.73f);
            floatParam->setValueNotifyingHost(floatParam->convertTo0to1(candidate));
        }
        else
        {
            return 0;
        }

        const float after = raw->load();
        return (std::abs(after - before) > entry.tolerance) ? 1 : 0;
    };

    auto mutateAll = [&]() -> int
    {
        int changed = 0;
        for (const auto& entry : expected)
            changed += mutateParam(entry);
        return changed;
    };

    const int startupMismatches = validateAll("startup");
    const auto* reverbRouting = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter("ReverbRouting"));
    const bool routingChoiceTextMatches =
        reverbRouting != nullptr && reverbRouting->choices.size() == 3 &&
        reverbRouting->choices[1] == juce::String(juce::CharPointer_UTF8("Echo \u2192 Reverb")) &&
        reverbRouting->choices[2] == juce::String(juce::CharPointer_UTF8("Reverb \u2192 Echo"));
    if (!routingChoiceTextMatches && firstMismatch.isEmpty())
        firstMismatch = "ReverbRouting UTF-8 choice text";

    const int startupSpatialPresetMatch =
        SpatialPresetCatalog::findMatchingPresetIndex(SpatialPresetCatalog::readSnapshot(vts));
    const int mutatedCount = mutateAll();

    processor.loadDefaultPreset();

    const int restoredMismatches = validateAll("restore");
    const int restoredSpatialPresetMatch =
        SpatialPresetCatalog::findMatchingPresetIndex(SpatialPresetCatalog::readSnapshot(vts));
    const bool spatialPresetMatchedDefault = (startupSpatialPresetMatch == 0) && (restoredSpatialPresetMatch == 0);
    if (!spatialPresetMatchedDefault && firstMismatch.isEmpty())
    {
        firstMismatch = "spatialPreset startup=" + juce::String(startupSpatialPresetMatch) +
                        " restore=" + juce::String(restoredSpatialPresetMatch);
    }

    const bool passed = (startupMismatches == 0) && (restoredMismatches == 0) && (mutatedCount > 0) &&
                        routingChoiceTextMatches && spatialPresetMatchedDefault && tapeNoiseVersionHintOk;

    juce::String result;
    result << "InitDefaultParameterContract:"
           << " startupMismatch=" << startupMismatches << " restoreMismatch=" << restoredMismatches
           << " mutated=" << mutatedCount << " routingText=" << (routingChoiceTextMatches ? 1 : 0)
           << " spatialPreset=" << startupSpatialPresetMatch << "->" << restoredSpatialPresetMatch
           << " tapeNoiseVersion="
           << (tapeNoiseParameter != nullptr ? juce::String(tapeNoiseParameter->getVersionHint()) : "missing")
           << " maxErr=" << juce::String(maxError, 6);
    if (!firstMismatch.isEmpty())
        result << " first=" << firstMismatch;
    result << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runSpatialPresetIntegrityTest(SpatialEchoAudioProcessor& processor)
{
    auto& vts = processor.getValueTreeState();
    const auto& presets = SpatialPresetCatalog::kDefinitions;
    struct ExpectedDefinition
    {
        const char* displayName;
        float width;
        float crossoverHz;
        int routingMode;
        int motionPattern;
        float motionRateHz;
        bool motionSync;
        int motionDivision;
        float motionDepth;
        int motionRetrig;
        int motionChoreo;
        float motionSpread;
    };
    constexpr std::array<ExpectedDefinition, 28> expectedDefinitions = {{
        {"Neutral", 0.50f, 150.0f, 0, 0, 0.35f, false, 3, 45.0f, 0, 0, 0.0f},
        {"Narrow Field", 0.22f, 150.0f, 0, 0, 0.35f, false, 3, 0.0f, 0, 0, 0.0f},
        {"Shallow Orbit 0.16 Hz", 0.22f, 240.0f, 0, 1, 0.16f, false, 3, 36.0f, 0, 0, 0.0f},
        {"Low-End Half-Note Orbit", 0.24f, 240.0f, 0, 1, 0.35f, true, 1, 50.0f, 1, 0, 0.0f},
        {"Half-Triplet Pendulum", 0.22f, 230.0f, 0, 2, 0.35f, true, 7, 47.0f, 1, 0, 0.0f},
        {"Orbit 0.18 Hz", 0.24f, 170.0f, 0, 1, 0.18f, false, 3, 53.0f, 0, 0, 0.0f},
        {"Wide Quarter-Note Orbit", 0.20f, 150.0f, 0, 1, 0.35f, true, 2, 78.0f, 1, 0, 0.0f},
        {"Quarter-Triplet Pendulum", 0.20f, 180.0f, 0, 2, 0.35f, true, 8, 75.0f, 1, 0, 0.0f},
        {"Wide Orbit 0.05 Hz", 0.22f, 160.0f, 0, 1, 0.05f, false, 3, 67.0f, 0, 0, 0.0f},
        {"Broad Half-Note Orbit", 0.21f, 160.0f, 0, 1, 0.35f, true, 1, 69.0f, 1, 0, 0.0f},
        {"Orbit 0.22 Hz", 0.23f, 180.0f, 0, 1, 0.22f, false, 3, 49.0f, 0, 0, 0.0f},
        {"Subtle Half-Note Orbit", 0.21f, 300.0f, 0, 1, 0.35f, true, 1, 39.0f, 1, 0, 0.0f},
        {"Light Whole-Note Breathe", 0.24f, 130.0f, 0, 4, 0.35f, true, 0, 46.0f, 1, 0, 0.0f},
        {"Shallow Orbit 0.11 Hz", 0.23f, 150.0f, 0, 1, 0.11f, false, 3, 42.0f, 0, 0, 0.0f},
        {"Pre-FX Wander 0.22 Hz", 0.22f, 150.0f, 1, 5, 0.22f, false, 3, 53.0f, 0, 0, 0.0f},
        {"Orbit 0.10 Hz", 0.24f, 140.0f, 0, 1, 0.10f, false, 3, 49.0f, 0, 0, 0.0f},
        {"Deep Wander 0.30 Hz", 0.25f, 140.0f, 0, 5, 0.30f, false, 3, 72.0f, 0, 0, 0.0f},
        {"Deep Wander 0.16 Hz", 0.22f, 320.0f, 0, 5, 0.16f, false, 3, 81.0f, 0, 0, 0.0f},
        {"Deep Orbit 0.35 Hz", 0.20f, 170.0f, 0, 1, 0.35f, false, 3, 92.0f, 0, 0, 0.0f},
        {"Figure 8 3.60 Hz", 0.22f, 200.0f, 0, 3, 3.60f, false, 3, 58.0f, 0, 0, 0.0f},
        {"Full Wander 1.20 Hz", 0.18f, 180.0f, 0, 5, 1.20f, false, 3, 100.0f, 0, 0, 0.0f},
        {"Pre-FX Whole-Note Orbit", 0.20f, 160.0f, 1, 1, 0.35f, true, 0, 100.0f, 1, 0, 0.0f},
        {"Pre-FX Half-Note Orbit", 0.20f, 160.0f, 1, 1, 0.35f, true, 1, 85.0f, 1, 0, 0.0f},
        {"Deep Half-Note Orbit", 0.21f, 170.0f, 0, 1, 0.35f, true, 1, 67.0f, 1, 0, 0.0f},
        {"Deep Whole-Note Breathe", 0.22f, 130.0f, 0, 4, 0.35f, true, 0, 61.0f, 1, 0, 0.0f},
        {"Dotted-Half Orbit", 0.20f, 300.0f, 0, 1, 0.35f, true, 12, 58.0f, 1, 0, 0.0f},
        {"Quarter-Note Figure 8", 0.18f, 210.0f, 0, 3, 0.35f, true, 2, 100.0f, 0, 0, 0.0f},
        {"Random-Phase Orbit", 0.22f, 170.0f, 0, 1, 2.40f, false, 3, 100.0f, 3, 0, 0.0f},
    }};

    int applyFailures = 0;
    int matchFailures = 0;
    int customDetectionFailures = 0;
    int nameFailures = 0;
    int categoryPrefixFailures = 0;
    int duplicateNameFailures = 0;
    int definitionFailures = 0;
    float maxAbsFloatError = 0.0f;

    auto trackError = [&](float actual, float expected)
    { maxAbsFloatError = std::max(maxAbsFloatError, std::abs(actual - expected)); };

    for (size_t i = 0; i < presets.size(); ++i)
    {
        const juce::String displayName(presets[i].displayName);
        if (i >= expectedDefinitions.size() || displayName != expectedDefinitions[i].displayName)
            ++nameFailures;
        if (displayName.containsChar(':'))
            ++categoryPrefixFailures;
        for (size_t j = 0; j < i; ++j)
        {
            if (displayName == presets[j].displayName)
                ++duplicateNameFailures;
        }
        if (i < expectedDefinitions.size())
        {
            const auto& preset = presets[i];
            const auto& expected = expectedDefinitions[i];
            const bool definitionMatches =
                SpatialPresetCatalog::approximatelyEqual(preset.width, expected.width, 0.0001f) &&
                SpatialPresetCatalog::approximatelyEqual(preset.crossoverHz, expected.crossoverHz, 0.0001f) &&
                preset.routingMode == expected.routingMode && preset.motionPattern == expected.motionPattern &&
                SpatialPresetCatalog::approximatelyEqual(preset.motionRateHz, expected.motionRateHz, 0.0001f) &&
                preset.motionSync == expected.motionSync && preset.motionDivision == expected.motionDivision &&
                SpatialPresetCatalog::approximatelyEqual(preset.motionDepth, expected.motionDepth, 0.0001f) &&
                preset.motionRetrig == expected.motionRetrig && preset.motionChoreo == expected.motionChoreo &&
                SpatialPresetCatalog::approximatelyEqual(preset.motionSpread, expected.motionSpread, 0.0001f);
            if (!definitionMatches)
                ++definitionFailures;
        }

        const int presetIndex = static_cast<int>(i);
        if (!SpatialPresetCatalog::applyPresetToParameters(vts, presetIndex, false))
        {
            ++applyFailures;
            continue;
        }

        const auto snapshot = SpatialPresetCatalog::readSnapshot(vts);
        const auto& expected = presets[i];
        const int matched = SpatialPresetCatalog::findMatchingPresetIndex(snapshot);
        if (matched != presetIndex)
            ++matchFailures;

        trackError(snapshot.width, expected.width);
        trackError(snapshot.crossoverHz, expected.crossoverHz);
        trackError(snapshot.motionRateHz, expected.motionRateHz);
        trackError(snapshot.motionDepth, expected.motionDepth);

        // Test that slightly modified depth doesn't match
        const float depthDelta = (expected.motionDepth <= 50.0f) ? 5.0f : -5.0f;
        const float customDepth = juce::jlimit(0.0f, 100.0f, expected.motionDepth + depthDelta);
        SpatialPresetCatalog::setParameterValue(vts, SpatialIDs::motion::kDepth, customDepth, false);
        const int customMatch = SpatialPresetCatalog::findMatchingPresetIndex(SpatialPresetCatalog::readSnapshot(vts));
        if (customMatch != -1)
            ++customDetectionFailures;
    }

    const bool passed = (presets.size() == expectedDefinitions.size()) && (nameFailures == 0) &&
                        (categoryPrefixFailures == 0) && (duplicateNameFailures == 0) && (definitionFailures == 0) &&
                        (applyFailures == 0) && (matchFailures == 0) && (customDetectionFailures == 0);

    juce::String result;
    result << "SpatialPresetIntegrity: presets=" << static_cast<int>(presets.size()) << " applyFail=" << applyFailures
           << " matchFail=" << matchFailures << " customFail=" << customDetectionFailures
           << " nameFail=" << nameFailures << " prefixes=" << categoryPrefixFailures
           << " duplicates=" << duplicateNameFailures << " definitionFail=" << definitionFailures
           << " maxErr=" << juce::String(maxAbsFloatError, 5) << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

}  // namespace SpatialEchoTests::System

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
