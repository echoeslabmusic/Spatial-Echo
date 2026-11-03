// SpatialEchoAudioProcessor: Main audio plugin processor: audio routing, parameter management, DSP graph
//   (echo/reverb/binaural).
// Realtime-safety: No allocation/locks in processBlock(); parameter updates via atomic queues.

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "DSP/Reverbprocessor.h"
#include "DSP/OouraFFT.h"
#include "DSP/dependencies/RbjBiquad.h"
#include "DSP/dependencies/ImprovedSpringReverb.h"
#include "DSP/HeadRatioConstants.h"
#include "DSP/SpatialMotionLfo.h"
#include "DSP/SpatialMotionTrajectory.h"
#include "DSP/TapeFlavors.h"
#include "Presets/FactoryPresetLibrary.h"
#include "ParameterWriteHelpers.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace
{
// APVTS toggle/bool parameters are normalised to [0, 1]; treat a value above the
// midpoint as "on". Used for the Power and Auto-Panic switches.
constexpr float kBoolParamOnThreshold = 0.5f;

struct ScopedProcessorSuspend
{
    explicit ScopedProcessorSuspend(juce::AudioProcessor& processorIn)
        : processor(processorIn)
    {
        wasSuspended = processor.isSuspended();
        if (!wasSuspended)
            processor.suspendProcessing(true);
    }

    ~ScopedProcessorSuspend()
    {
        if (!wasSuspended)
            processor.suspendProcessing(false);
    }

    juce::AudioProcessor& processor;
    bool wasSuspended = false;
};

constexpr const char* kUserPresetFileExtension = ".spatialEchopreset";
constexpr const char* kUserPresetWrapperTag = "SpatialEchoPreset";

float outputGainParamToLinear(float outputGainParam)
{
    const float outputGainDb = (juce::jlimit(0.0f, 1.0f, outputGainParam) - 0.5f) * 24.0f;
    return juce::Decibels::decibelsToGain(outputGainDb);
}

// Materialize defaults for post-v1 parameters before replacing host or user-
// preset state. This makes sessions that lack them deterministic regardless of
// APVTS's missing-parameter behavior and prevents a reused instance from
// inheriting a value the saved state never contained.
void migrateMissingExtensionParameters(juce::ValueTree& state)
{
    if (!state.isValid())
        return;

    auto materializeDefault = [&state](const char* parameterId, int defaultValue)
    {
        if (state.getChildWithProperty("id", parameterId).isValid())
            return;

        juce::ValueTree parameterState("PARAM");
        parameterState.setProperty("id", parameterId, nullptr);
        parameterState.setProperty("value", defaultValue, nullptr);
        state.appendChild(parameterState, nullptr);
    };

    materializeDefault(SpatialIDs::motion::kLongDivision, SpatialIDs::motion::kLongDivisionDefault);
    materializeDefault(SpatialIDs::motion::kLongVariantDivision, SpatialIDs::motion::kLongVariantDivisionDefault);
    materializeDefault("TapeNoiseEnabled", 1);
}

// Version 1.0.0 sessions and presets can carry a SofaPath root property naming
// an external HRTF file. The HRTF always comes from the embedded data set, so
// the property is dropped on load and never written back.
void removeExternalHrtfPath(juce::ValueTree& state)
{
    state.removeProperty("SofaPath", nullptr);
}

// Applies every PARAM child of a serialized state tree through the host-
// notifying parameter path. Unlike AudioProcessorValueTreeState::replaceState
// (which clears the UndoManager history), these writes are flushed to the
// ValueTree as undoable changes, so an accidental preset load can be undone.
void applyPresetParameterValuesFromState(juce::AudioProcessorValueTreeState& treeState,
                                         const juce::ValueTree& sourceState)
{
    for (int i = 0; i < sourceState.getNumChildren(); ++i)
    {
        const auto child = sourceState.getChild(i);
        if (!child.hasType("PARAM"))
            continue;

        const auto parameterId = child.getProperty("id").toString();
        if (auto* parameter = treeState.getParameter(parameterId))
        {
            const float plainValue = static_cast<float>(child.getProperty("value"));
            parameter->setValueNotifyingHost(parameter->convertTo0to1(plainValue));
        }
    }
}

juce::File getUserPresetDirectory()
{
#if SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    if (const char* testPresetDir = std::getenv("SPATIAL_ECHO_TEST_PRESET_DIR"))
    {
        const juce::String overridePath = juce::String::fromUTF8(testPresetDir).trim();
        if (overridePath.isNotEmpty())
        {
            if (juce::File::isAbsolutePath(overridePath))
                return juce::File(overridePath);

            return juce::File::getCurrentWorkingDirectory().getChildFile(overridePath);
        }
    }
#endif

    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Spatial Echo")
        .getChildFile("Presets");
}

juce::String sanitizePresetName(juce::String name)
{
    name = name.trim();
    if (name.isEmpty())
        return {};

    name = name.replaceCharacters("\\/:*?\"<>|", "_________");
    while (name.contains("  "))
        name = name.replace("  ", " ");
    return name.trim();
}

juce::File getPresetFileForName(const juce::String& name)
{
    const auto safeName = sanitizePresetName(name);
    if (safeName.isEmpty())
        return {};
    return getUserPresetDirectory().getChildFile(safeName + kUserPresetFileExtension);
}

bool isSupportedStateSchema(const juce::ValueTree& state)
{
    // Existing current-format unversioned files are accepted as schema 1.
    // Do not coerce malformed strings (e.g. "1future") into an accepted integer.
    return !state.hasProperty("stateSchemaVersion") || state.getProperty("stateSchemaVersion").toString() == "1";
}

juce::ValueTree readPresetStateFromFile(const juce::File& presetFile, const juce::Identifier& stateTag)
{
    auto xml = juce::parseXML(presetFile);
    if (xml == nullptr)
    {
        DBG("readPresetStateFromFile: could not parse XML from " + presetFile.getFullPathName());
        return {};
    }

    // Official preset contract: files carry only the wrapper root with the
    // state child inside. Anything else is not a Spatial Echo preset.
    juce::ValueTree state;
    if (xml->hasTagName(kUserPresetWrapperTag))
    {
        if (auto* stateXml = xml->getChildByName(stateTag.toString()))
            state = juce::ValueTree::fromXml(*stateXml);
        else
        {
            DBG("readPresetStateFromFile: preset wrapper is missing expected state child '" + stateTag.toString() +
                "' in " + presetFile.getFileName());
            return {};
        }
    }
    else
    {
        DBG("readPresetStateFromFile: unexpected root tag '" + xml->getTagName() + "' (expected '" +
            juce::String(kUserPresetWrapperTag) + "') in " + presetFile.getFileName());
        return {};
    }

    return isSupportedStateSchema(state) ? state : juce::ValueTree();
}

juce::String readPresetNameFromFile(const juce::File& presetFile)
{
    if (auto xml = juce::parseXML(presetFile))
    {
        if (xml->hasTagName(kUserPresetWrapperTag))
        {
            const auto name = xml->getStringAttribute("name").trim();
            if (name.isNotEmpty())
                return name;
        }
    }
    return presetFile.getFileNameWithoutExtension();
}

bool writePresetFile(const juce::ValueTree& state, const juce::File& presetFile, const juce::String& name)
{
    if (!state.isValid())
        return false;

    const auto safeName = sanitizePresetName(name);
    if (safeName.isEmpty())
        return false;

    auto parentDir = presetFile.getParentDirectory();
    if (!parentDir.exists() && !parentDir.createDirectory())
        return false;

    auto xml = state.createXml();
    if (xml == nullptr)
        return false;

    auto wrapper = std::make_unique<juce::XmlElement>(kUserPresetWrapperTag);
    wrapper->setAttribute("name", safeName);
    wrapper->addChildElement(xml.release());
    // Do not replace the existing preset until the entire new file has been
    // written and flushed successfully. JUCE's replaceWithText does not check
    // its temporary-file write result before committing that file.
    juce::TemporaryFile temporaryFile(presetFile, juce::TemporaryFile::useHiddenFile);
    {
        juce::FileOutputStream stream(temporaryFile.getFile());
        if (!stream.openedOk() || !stream.writeText(wrapper->toString(), false, false, "\n"))
            return false;
        stream.flush();
        if (stream.getStatus().failed())
            return false;
    }
    return temporaryFile.overwriteTargetFileWithTemporary();
}

int findFactoryPresetIndexByName(juce::StringRef name)
{
    const juce::String target(name);
    for (size_t i = 0; i < FactoryPresetLibrary::kFactoryPresets.size(); ++i)
    {
        if (target.compareIgnoreCase(FactoryPresetLibrary::kFactoryPresets[i].name) == 0)
            return static_cast<int>(i);
    }

    return -1;
}

float getFactoryPresetMixFloor(const char* categoryId)
{
    return FactoryPresetLibrary::getFactoryPresetCategoryMixFloor(categoryId);
}
}  // namespace

// NOTE [DESIGN CONTEXT]:
// Comments tagged with "Design" describe intentional tuning choices
// (routing, ranges, tapers, noise, etc.). Update rationale if you change them.

namespace
{
// Design: RepeatRate uses this non-linear taper to give more resolution at short delays.
// Always call shapeRepeatRate() before mapping to milliseconds.
inline float shapeRepeatRate(float x)
{
    return std::pow(juce::jlimit(0.0f, 1.0f, x), 0.6f);
}

// Design: map the public Feedback travel directly to measured repeat decay.
// The declared anchors keep the useful decay region broad, reserve the top
// of the control for bounded near-sustain, and make interpolation auditable.
struct FeedbackMapAnchor
{
    float raw;
    float loopGain;
};

// End-to-end calibration against measured repeat decay after the tape and
// tone paths have settled. Values are the single source of loop gain; the
// public parameter remains 0..1 and existing automation is unchanged.
inline constexpr std::array<FeedbackMapAnchor, 6> kFeedbackMap{
    {{0.00f, 0.000f}, {0.25f, 0.374f}, {0.50f, 0.629f}, {0.75f, 0.838f}, {0.90f, 1.025f}, {1.00f, 1.040f}}};

inline float mapFeedbackToLoopGain(float raw) noexcept
{
    const float clamped = std::clamp(std::isfinite(raw) ? raw : 0.0f, 0.0f, 1.0f);
    for (size_t index = 1; index < kFeedbackMap.size(); ++index)
    {
        const auto& high = kFeedbackMap[index];
        if (clamped > high.raw)
            continue;

        const auto& low = kFeedbackMap[index - 1];
        const float span = high.raw - low.raw;
        const float position = span > 0.0f ? (clamped - low.raw) / span : 0.0f;
        return low.loopGain + position * (high.loopGain - low.loopGain);
    }

    return kFeedbackMap.back().loopGain;
}

inline float mapInputLevelToDb(float raw)
{
    const float clamped = juce::jlimit(0.0f, 1.0f, raw);
    if (clamped <= 0.5f)
        return juce::jmap(clamped, 0.0f, 0.5f, -60.0f, 0.0f);
    return juce::jmap(clamped, 0.5f, 1.0f, 0.0f, 12.0f);
}

struct PreampPresetInfo
{
    const char* name;
    float input;
    float bass;
    float treble;
};

static constexpr PreampPresetInfo kPreampPresets[] = {{"Clean", 0.50f, 0.50f, 0.50f},
                                                      {"Warm", 0.55f, 0.65f, 0.40f},
                                                      {"Bright", 0.55f, 0.40f, 0.65f},
                                                      {"Drive", 0.70f, 0.50f, 0.50f},
                                                      {"Lo-Fi", 0.75f, 0.70f, 0.25f}};
}  // namespace

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
float SpatialEchoAudioProcessor::debugMapFeedbackToLoopGain(float raw) noexcept
{
    return mapFeedbackToLoopGain(raw);
}
#endif

float SpatialEchoAudioProcessor::getMotionNoteDivisionMultiplier(int divisionIndex)
{
    constexpr int baseCount = static_cast<int>(sizeof(noteDivisionMultipliers) / sizeof(noteDivisionMultipliers[0]));
    constexpr int slowCount =
        static_cast<int>(sizeof(slowMotionDivisionMultipliers) / sizeof(slowMotionDivisionMultipliers[0]));
    static_assert(baseCount + slowCount == SpatialIDs::motion::kDivisionMenuCount);

    const int clamped = juce::jlimit(0, SpatialIDs::motion::kDivisionMenuCount - 1, divisionIndex);
    if (clamped < baseCount)
        return noteDivisionMultipliers[clamped];
    return slowMotionDivisionMultipliers[clamped - baseCount];
}

//==============================================================================
SpatialEchoAudioProcessor::SpatialEchoAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
    : AudioProcessor(BusesProperties()
#if !JucePlugin_IsMidiEffect
#if !JucePlugin_IsSynth
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
#endif
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)
#endif
                         )
    , treestate(*this, &undoManager, "parameters", params())
#endif
{
    // Set this processor object as a listener to the value tree
    treestate.state.addListener(this);

    // Add parameters to treestate
    populateParameters();

    // Clear atomic flag so that parameters are saved to internal variables
    changesApplied.clear();

    // Capture default parameter state for "Init" preset recall
    defaultStateSnapshot = treestate.copyState();

    // Fresh instances open on the first factory preset instead of the INIT
    // state. Direct
    // construction (wrapperType_Undefined: headless tests, offline render
    // tools) keeps INIT defaults so measurement baselines stay INIT-anchored.
    // Host session restore still wins because setStateInformation replaces
    // the whole state after construction.
    if (wrapperType != juce::AudioProcessor::wrapperType_Undefined)
    {
        const auto& firstPreset = FactoryPresetLibrary::kFactoryPresets.front();
        loadFactoryPreset(firstPreset.name);

        // Stamp the preset identity the editor persists and navigates from
        // (PresetSelectionType::Factory == 1); without it the instance
        // sounds like the first preset while identifying as Init and
        // Previous/Next starts from the wrong position.
        treestate.state.setProperty("activePresetName", juce::String(firstPreset.name), nullptr);
        treestate.state.setProperty("activePresetType", 1, nullptr);

        // The startup load must not be user-undoable: force the APVTS
        // parameter->ValueTree flush that would otherwise land in the
        // UndoManager from the message-thread timer, then drop it.
        treestate.copyState();
        undoManager.clearUndoHistory();
    }

    if (juce::MessageManager::getInstanceWithoutCreating() != nullptr)
        startTimerHz(60);
}

SpatialEchoAudioProcessor::~SpatialEchoAudioProcessor()
{
    stopTimer();
}

//==============================================================================
const juce::String SpatialEchoAudioProcessor::getName() const
{
#if JUCE_DEBUG
    return juce::String(JucePlugin_Name) + " [DEBUG]";
#else
    return JucePlugin_Name;
#endif
}

bool SpatialEchoAudioProcessor::acceptsMidi() const
{
#if JucePlugin_WantsMidiInput
    return true;
#else
    return false;
#endif
}

bool SpatialEchoAudioProcessor::producesMidi() const
{
#if JucePlugin_ProducesMidiOutput
    return true;
#else
    return false;
#endif
}

bool SpatialEchoAudioProcessor::isMidiEffect() const
{
#if JucePlugin_IsMidiEffect
    return true;
#else
    return false;
#endif
}

double SpatialEchoAudioProcessor::getTailLengthSeconds() const
{
    // Max feedback intentionally supports self-oscillation, so hosts should not
    // trim offline/freeze renders based on a finite decay estimate.
    return std::numeric_limits<double>::infinity();
}

int SpatialEchoAudioProcessor::getNumPrograms()
{
    return 1;  // NB: some hosts don't cope very well if you tell them there are 0 programs,
               // so this should be at least 1, even if you're not really implementing programs.
}

int SpatialEchoAudioProcessor::getCurrentProgram()
{
    return 0;
}

void SpatialEchoAudioProcessor::setCurrentProgram(int index)
{
    juce::ignoreUnused(index);
}

const juce::String SpatialEchoAudioProcessor::getProgramName(int index)
{
    if (index == 0)
        return "Default";
    return {};
}

void SpatialEchoAudioProcessor::changeProgramName(int index, const juce::String& newName)
{
    juce::ignoreUnused(index, newName);
}

//==============================================================================
void SpatialEchoAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    // prepare runs without an active callback. Seed the fallback from a complete
    // formation before the first callback can overlap a code-owned edit.
    {
        const juce::ScopedLock writerLock(spatialWriterLock);
        readSpatialPositionsForAudio();
    }

    // Use this method as the place to do any pre-playback
    // initialisation that you need..

    // Clear atomic flag so that parameters are saved to internal variables
    changesApplied.clear();

    lastSampleRate = sampleRate;
    autoPanicHeldSamples = 0;
    autoPanicCooldownSamples = 0;
    spatialMotionPhaseCycles = 0.0;
    spatialMotionRandomStep = -1;
    spatialMotionRandomPhaseOffsetCycles = 0.0;
    spatialMotionRandomPhasePrimed = false;
    spatialMotionNoteOnThisBlock = false;
    spatialMotionSmootherPrimed = false;
    spatialMotionSmoothedAz = 0.0f;
    spatialMotionSmoothedEl = 0.0f;
    spatialMotionSmoothedDist = 0.0f;
    manualSpatialActivitySamplesRemaining = 0;
    manualSpatialActivityRequested.store(false, std::memory_order_release);
    spatialSourceTrackingInitialized = false;
    spatialMotionPreviewPhaseCyclesForUi.store(0.0, std::memory_order_relaxed);
    spatialMotionRateHzForUi.store(0.35f, std::memory_order_relaxed);
    spatialMotionBlockDurationSecForUi.store(0.0f, std::memory_order_relaxed);
    spatialMotionModAzForUi.store(0.0f, std::memory_order_relaxed);
    spatialMotionModElForUi.store(0.0f, std::memory_order_relaxed);
    spatialMotionModDistForUi.store(0.0f, std::memory_order_relaxed);

    const int outputChannels = juce::jmax(1, getTotalNumOutputChannels());
    constexpr int kMaxExpectedBlockSize = 16384;
    const int safeBlockSize = std::max(samplesPerBlock, kMaxExpectedBlockSize);

    // Reset the delay-owned record saturator (8x <=48 kHz, 4x above).
    // Size the DSP to the host's DECLARED block size, not the padded maximum:
    // components that touch prepared-size regions per block (filter contexts,
    // buffer clears) would otherwise do up to 16384/block work for 64-sample
    // blocks (measured 382us vs 6us per binaural voice). Oversized blocks from
    // misbehaving hosts are chunk-split by EchoesModel::ProcessBuffer.
    const int dspBlockSize = juce::jlimit(64, kMaxExpectedBlockSize, samplesPerBlock);
    echomodel.Reset(static_cast<float>(sampleRate), outputChannels, dspBlockSize);

    reloadDefaultHrtf();

    // Preallocate for a conservative maximum so processBlock never grows vectors on the audio thread.
    processingBuffer.resize(static_cast<size_t>(outputChannels));
    for (auto& ch : processingBuffer)
        ch.resize(static_cast<size_t>(safeBlockSize));
    scopeMixBuf.resize(static_cast<size_t>(safeBlockSize), 0.0f);

    outputProtectionStage.prepare(outputChannels);

    bypassDryScratch.resize(static_cast<size_t>(outputChannels));
    for (auto& ch : bypassDryScratch)
        ch.assign(static_cast<size_t>(safeBlockSize), 0.0f);
    bypassRampStep = static_cast<float>(1.0 / std::max(1.0, kBypassRampTimeSec * sampleRate));
    bypassRampGain = bypassRampTarget = lastBlockWasBypassed ? 1.0f : 0.0f;

    const float initialOutputGainParam = (OutputGain != nullptr) ? OutputGain->load() : 0.5f;
    const float initialOutputGain = outputGainParamToLinear(initialOutputGainParam);
    outputGainSmoother.reset(sampleRate, kOutputGainSmoothingTimeSec);
    outputGainSmoother.setCurrentAndTargetValue(initialOutputGain);
    outputGainSmootherPrepared = true;

    // Start the protection crossfade already settled on the current parameter
    // state so the first block after prepare does not ramp audibly.
    const bool initialOutputProtect = (OutputProtection != nullptr) ? (OutputProtection->load() > 0.5f) : true;
    outputProtectionMixSmoother.reset(sampleRate, kOutputProtectionSmoothingTimeSec);
    outputProtectionMixSmoother.setCurrentAndTargetValue(initialOutputProtect ? 1.0f : 0.0f);
    outputProtectionSmootherPrepared = true;

    // Final protection and protection-off paths share constant integer latency.
    // Feedback-loop oversampling remains an intentional part of the echo sound.
    setLatencySamples(OutputProtectionStage::latencySamples);
}

void SpatialEchoAudioProcessor::releaseResources()
{
    // When playback stops, you can use this as an opportunity to free up any
    // spare memory, etc.
    outputProtectionStage.release();
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool SpatialEchoAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
#if JucePlugin_IsMidiEffect
    juce::ignoreUnused(layouts);
    return true;
#else
    // This is the place where you check if the layout is supported.
    // In this template code we only support mono or stereo.
    // Some plugin hosts, such as certain GarageBand versions, will only
    // load plugins that support stereo bus layouts.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono() &&
        layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // This checks if the input layout matches the output layout
#if !JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
#endif

    return true;
#endif
}
#endif

void SpatialEchoAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    if (lastBlockWasBypassed)
    {
        lastBlockWasBypassed = false;
        bypassRampTarget = 0.0f;
    }

    processActiveBlock(buffer, midiMessages);
    applyBypassRamp(buffer);
}

void SpatialEchoAudioProcessor::processActiveBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    const int numSamples = buffer.getNumSamples();

    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    // In case we have more outputs than inputs, this code clears any output
    // channels that didn't contain input data, (because these aren't
    // guaranteed to be empty - they may contain garbage).
    // This is here to avoid people getting screaming feedback
    // when they first compile a plugin, but obviously you don't need to keep
    // this code if your algorithm always overwrites all the output channels.
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    // Upstream plug-ins and hosts occasionally deliver NaN, Inf, or absurd
    // spikes. One such sample would latch every recursive filter and feedback
    // loop, and with Auto-Panic off (the default) the output scrub would then
    // hold the whole unit silent until it is re-prepared. Normal audio is
    // untouched.
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        float* data = buffer.getWritePointer(channel);
        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float x = data[sample];
            if (!(std::abs(x) <= kMaxInputMagnitude))  // also catches NaN
                data[sample] = std::isfinite(x) ? std::copysign(kMaxInputMagnitude, x) : 0.0f;
        }
    }

    // Preserve raw input history for latency-correct host bypass transitions,
    // and keep this block's delayed dry for the bypass crossfade.
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        const auto channelIndex = static_cast<size_t>(channel);
        float* dry = channelIndex < bypassDryScratch.size() &&
                             bypassDryScratch[channelIndex].size() >= static_cast<size_t>(numSamples)
                         ? bypassDryScratch[channelIndex].data()
                         : nullptr;
        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float delayed = outputProtectionStage.pushBypassInput(channel, buffer.getSample(channel, sample));
            if (dry != nullptr)
                dry[sample] = delayed;
        }
    }

    // Read host tempo information from playhead (JUCE 7+ API)
    // With BPM hysteresis to prevent jitter from minor fluctuations
    if (auto* localPlayHead = getPlayHead())
    {
        if (auto positionInfo = localPlayHead->getPosition())
        {
            if (auto bpm = positionInfo->getBpm())
            {
                double newBpm = *bpm;
                // Only update if BPM change exceeds hysteresis threshold
                // This prevents zipper noise from minor host BPM fluctuations
                if (newBpm > 0.0 && std::abs(newBpm - hostBpm) > bpmHysteresisThreshold)
                {
                    hostBpm = newBpm;
                    lastValidBpm = newBpm;  // Cache valid BPM
                }
                else if (newBpm <= 0.0)
                {
                    // Host returned invalid BPM - use cached value
                    hostBpm = lastValidBpm;
                }
            }
            // Track PPQ position for beat-accurate sync
            if (auto ppq = positionInfo->getPpqPosition())
                hostPpqPosition = *ppq;
            hostIsPlaying = positionInfo->getIsPlaying();
        }
    }
    else
    {
        // Standalone mode - use cached BPM (default 120)
        hostBpm = lastValidBpm;
    }

    // Spatial MIDI input, read before the parameter update. MIDI reaches the
    // processor only in the Standalone build: the plug-in formats declare no
    // MIDI input, so hosts never deliver any.
    processSpatialMidiCC(midiMessages);

    // Update dsp object with plugin parameter values
    updateParameters(numSamples);

    // Power state: turning power OFF fades out smoothly, then bypasses all
    // processing.
    bool powerOn = (Panic != nullptr) ? (*Panic > kBoolParamOnThreshold) : true;

    // Detect Power switch transitions for delay buffer management
    if (Panic != nullptr)
    {
        float currentPowerValue = *Panic;
        if (currentPowerValue <= kBoolParamOnThreshold && lastKillValue > kBoolParamOnThreshold)
        {
            // Power turned OFF - start fade-out and trigger delay kill
            powerFading = true;
            powerFadeEnvelope = 1.0f;
            echomodel.triggerKill();
        }
        else if (currentPowerValue > kBoolParamOnThreshold && lastKillValue <= kBoolParamOnThreshold)
        {
            // Power turned back ON - reset fade envelope and cancel delay kill
            powerFading = false;
            powerFadeEnvelope = 1.0f;
            echomodel.cancelKill();
        }
        lastKillValue = currentPowerValue;
    }

    // If power is OFF and fade is complete, clear buffer and skip all DSP processing
    if (!powerOn && !powerFading)
    {
        // Drain output history while the powered-off unit remains silent.
        for (int channel = 0; channel < totalNumOutputChannels; ++channel)
            for (int sample = 0; sample < numSamples; ++sample)
                outputProtectionStage.process(channel, 0.0f, 0.0f);
        buffer.clear();
        return;
    }

    // Get write pointer to channels
    auto bufferpointer = buffer.getArrayOfWritePointers();

    bool hasCapacity = (processingBuffer.size() >= static_cast<size_t>(totalNumOutputChannels));
    for (int i = 0; i < totalNumOutputChannels; ++i)
    {
        if (!hasCapacity)
            break;
        hasCapacity = processingBuffer[static_cast<size_t>(i)].size() >= static_cast<size_t>(numSamples);
    }

    // Guard against unexpected host behavior without allocating on the audio thread.
    if (!hasCapacity)
    {
        jassertfalse;
        buffer.clear();
        return;
    }

    // Apply audio to processing buffer
    for (int channel = 0; channel < totalNumOutputChannels; channel++)
    {
        auto& processingChannel = processingBuffer[static_cast<size_t>(channel)];
        for (int sample = 0; sample < numSamples; sample++)
        {
            // if mono stereo apply only input channel to both output channels
            if (totalNumInputChannels == 1)
                processingChannel[static_cast<size_t>(sample)] = bufferpointer[0][sample];
            else
                processingChannel[static_cast<size_t>(sample)] = bufferpointer[channel][sample];
        }
    }

    echomodel.ProcessBuffer(processingBuffer, buffer.getNumSamples());

    // Auto-panic watchdog: taps the post-model bus before output gain and
    // protection. `buffer` still holds the untouched host input here (it is
    // only overwritten in the write-out loop below), so it doubles as the
    // input-peak reference for the runaway gate.
    runAutoPanicWatchdog(buffer, numSamples, totalNumOutputChannels);

    // Output gain from parameter (0-1 maps to -12 to +12 dB, 0.5 = unity)
    const float outputGainParam = (OutputGain != nullptr) ? OutputGain->load() : 0.5f;
    const float outputGain = outputGainParamToLinear(outputGainParam);
    if (!outputGainSmootherPrepared)
    {
        const double sr = (lastSampleRate > 1.0) ? lastSampleRate : 48000.0;
        outputGainSmoother.reset(sr, kOutputGainSmoothingTimeSec);
        outputGainSmoother.setCurrentAndTargetValue(outputGain);
        outputGainSmootherPrepared = true;
    }
    outputGainSmoother.setTargetValue(outputGain);
    const bool outputProtectOn = (OutputProtection != nullptr) ? (OutputProtection->load() > 0.5f) : true;
    if (!outputProtectionSmootherPrepared)
    {
        const double sr = (lastSampleRate > 1.0) ? lastSampleRate : 48000.0;
        outputProtectionMixSmoother.reset(sr, kOutputProtectionSmoothingTimeSec);
        outputProtectionMixSmoother.setCurrentAndTargetValue(outputProtectOn ? 1.0f : 0.0f);
        outputProtectionSmootherPrepared = true;
    }
    outputProtectionMixSmoother.setTargetValue(outputProtectOn ? 1.0f : 0.0f);

    // Calculate power fade step per sample
    float fadeStep = 0.0f;
    if (powerFading)
    {
        fadeStep = 1.0f / (POWER_FADE_TIME_MS * 0.001f * static_cast<float>(lastSampleRate));
    }

    // Apply processed audio to buffer write pointer with output level and power fade.
    // Fade envelope advances once per sample so all channels stay phase-aligned.
    for (int sample = 0; sample < buffer.getNumSamples(); sample++)
    {
        float finalGain = outputGainSmoother.getNextValue();
        if (powerFading)
        {
            finalGain *= powerFadeEnvelope;
            powerFadeEnvelope = std::max(0.0f, powerFadeEnvelope - fadeStep);
        }

        // Advance the protection crossfade once per sample so every channel
        // stays phase-aligned, exactly as finalGain above does.
        const float protectionMix = outputProtectionMixSmoother.getNextValue();

        for (int channel = 0; channel < totalNumOutputChannels; channel++)
        {
            float sampleOut = processingBuffer[static_cast<size_t>(channel)][static_cast<size_t>(sample)] * finalGain;
            if (!std::isfinite(sampleOut))
                sampleOut = 0.0f;
            sampleOut = outputProtectionStage.process(channel, sampleOut, protectionMix);
            bufferpointer[channel][sample] = sampleOut;
        }
    }

    if (powerFading && powerFadeEnvelope <= 0.0f)
    {
        powerFadeEnvelope = 0.0f;
        powerFading = false;
    }

    // Push mono mix-down of final output to scope FIFO (UI oscilloscope tap)
    if (totalNumOutputChannels > 0)
    {
        const int numOut = buffer.getNumSamples();
        const float chanScale = 1.0f / static_cast<float>(totalNumOutputChannels);

        // Keep the scope tap allocation-free on the audio thread.
        const int scopeSamplesToWrite = juce::jmin(numOut, static_cast<int>(scopeMixBuf.size()));
        for (int s = 0; s < scopeSamplesToWrite; ++s)
        {
            float sum = 0.0f;
            for (int ch = 0; ch < totalNumOutputChannels; ++ch)
                sum += bufferpointer[ch][s];
            scopeMixBuf[static_cast<size_t>(s)] = sum * chanScale;
        }

        if (scopeSamplesToWrite > 0)
            scopeDataFifo.push(scopeMixBuf.data(), scopeSamplesToWrite);
    }
}

void SpatialEchoAudioProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    if (!lastBlockWasBypassed)
    {
        lastBlockWasBypassed = true;
        bypassRampTarget = 1.0f;
    }

    // Still fading out of the processed sound: keep processing and blend.
    if (bypassRampGain < 1.0f)
    {
        processActiveBlock(buffer, midiMessages);
        applyBypassRamp(buffer);
        return;
    }

    juce::ScopedNoDenormals noDenormals;
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        auto* samples = buffer.getWritePointer(channel);
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const float input = channel < getTotalNumInputChannels() ? samples[sample] : 0.0f;
            // Keep protection history current while the host bypasses the effect.
            outputProtectionStage.process(channel, input, 0.0f);
            samples[sample] = outputProtectionStage.pushBypassInput(channel, input);
        }
    }
}

void SpatialEchoAudioProcessor::applyBypassRamp(juce::AudioBuffer<float>& buffer) noexcept
{
    // The end of every ramp step snaps the gain onto the target once it is
    // within kBypassRampSettleTolerance, so a settled ramp compares exactly.
    if (std::abs(bypassRampGain - bypassRampTarget) < kBypassRampSettleTolerance)
        return;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = std::min(buffer.getNumChannels(), static_cast<int>(bypassDryScratch.size()));
    const bool hasDry = numChannels > 0 && bypassDryScratch.front().size() >= static_cast<size_t>(numSamples);
    if (!hasDry)
    {
        bypassRampGain = bypassRampTarget;
        return;
    }

    const float direction = bypassRampTarget > bypassRampGain ? 1.0f : -1.0f;
    for (int sample = 0; sample < numSamples; ++sample)
    {
        bypassRampGain = juce::jlimit(0.0f, 1.0f, bypassRampGain + direction * bypassRampStep);
        for (int channel = 0; channel < numChannels; ++channel)
        {
            float* out = buffer.getWritePointer(channel);
            const float dry = bypassDryScratch[static_cast<size_t>(channel)][static_cast<size_t>(sample)];
            out[sample] += bypassRampGain * (dry - out[sample]);
        }
    }

    if (std::abs(bypassRampGain - bypassRampTarget) < kBypassRampSettleTolerance)
        bypassRampGain = bypassRampTarget;
}

//==============================================================================
void SpatialEchoAudioProcessor::runAutoPanicWatchdog(const juce::AudioBuffer<float>& inputBuffer, int numSamples,
                                                     int numChannels)
{
    // Thresholds and rationale live with the constants in PluginProcessor.h.
    const bool enabled = (AutoPanic != nullptr) && (*AutoPanic > kBoolParamOnThreshold);
    if (!enabled)
    {
        autoPanicHeldSamples = 0;
        return;
    }

    if (autoPanicCooldownSamples > 0)
    {
        autoPanicCooldownSamples = std::max(0, autoPanicCooldownSamples - numSamples);
        return;
    }

    // One pass: block peak plus sum of magnitudes. A single NaN/Inf anywhere
    // poisons the sum, so one isfinite() check covers the whole block.
    float busPeak = 0.0f;
    float busAbsSum = 0.0f;
    const int scanChannels = juce::jmin(numChannels, static_cast<int>(processingBuffer.size()));
    for (int channel = 0; channel < scanChannels; ++channel)
    {
        const float* data = processingBuffer[static_cast<size_t>(channel)].data();
        for (int s = 0; s < numSamples; ++s)
        {
            const float magnitude = std::abs(data[s]);
            busAbsSum += magnitude;
            busPeak = std::max(busPeak, magnitude);
        }
    }

    bool trigger = false;
    bool hardClear = false;

    if (!std::isfinite(busAbsSum))
    {
        trigger = true;
        hardClear = true;
    }
    else
    {
        float inputPeak = 0.0f;
        const int inputChannels = juce::jmin(numChannels, inputBuffer.getNumChannels());
        for (int channel = 0; channel < inputChannels; ++channel)
            inputPeak = std::max(inputPeak, inputBuffer.getMagnitude(channel, 0, numSamples));
        if (!std::isfinite(inputPeak))
            inputPeak = 0.0f;  // broken input: judge the bus on its own

        const float runawayNormalization = std::max(1.0f, echomodel.getAutoPanicRunawayMonitorNormalization());
        const float runawayPeak = busPeak / runawayNormalization;
        const bool runaway = runawayPeak > kAutoPanicPeakThreshold && runawayPeak > inputPeak * kAutoPanicInputRatio;
        if (runaway)
        {
            autoPanicHeldSamples += numSamples;
            const float sampleRate = (lastSampleRate > 1.0) ? static_cast<float>(lastSampleRate) : 48000.0f;
            trigger = autoPanicHeldSamples >= static_cast<int>(kAutoPanicHoldSeconds * sampleRate);
        }
        else
        {
            autoPanicHeldSamples = 0;  // must hold continuously; spikes don't accumulate
        }
    }

    if (trigger)
    {
        echomodel.autoPanicFlush(!hardClear);
        autoPanicHeldSamples = 0;
        const float sampleRate = (lastSampleRate > 1.0) ? static_cast<float>(lastSampleRate) : 48000.0f;
        autoPanicCooldownSamples = static_cast<int>(kAutoPanicCooldownSeconds * sampleRate);
        autoPanicEvents.fetch_add(1, std::memory_order_relaxed);
    }
}

//==============================================================================
juce::AudioProcessorEditor* SpatialEchoAudioProcessor::createEditor()
{
    return new SpatialEchoAudioProcessorEditor(*this);
}

void SpatialEchoAudioProcessor::flushPendingHostUpdatesForUi()
{
    jassert(juce::MessageManager::getInstanceWithoutCreating() == nullptr ||
            juce::MessageManager::getInstanceWithoutCreating()->isThisTheMessageThread());
    flushPendingHostParameterUpdates();
}

//==============================================================================
void SpatialEchoAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // A host may save from a worker thread while processing continues. Saving
    // must not notify the host, consume queued writes, or reset live DSP caches.
    auto state = createStateSnapshot();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    if (xml != nullptr)
        copyXmlToBinary(*xml, destData);
}

void SpatialEchoAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    ScopedSpatialParameterBatch spatialBatch(*this);
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
    if (xmlState != nullptr && xmlState->hasTagName(treestate.state.getType()))
    {
        ScopedProcessorSuspend scopedSuspend(*this);
        auto newState = juce::ValueTree::fromXml(*xmlState);
        if (!isSupportedStateSchema(newState))
            return;
        migrateMissingExtensionParameters(newState);
        removeExternalHrtfPath(newState);

        treestate.replaceState(newState);
        syncPresetTrackingFromState();

        // Guarantee the first processBlock after state-load performs a full
        // parameter update (slow-update path included).  If replaceState
        // restored identical values the ValueTree listener may not fire,
        // leaving changesApplied set; clearing it here is always safe.
        changesApplied.clear();

        reloadDefaultHrtf();
    }
}

//==============================================================================
juce::StringArray SpatialEchoAudioProcessor::getFactoryPresetNames() const
{
    juce::StringArray names;
    names.ensureStorageAllocated(static_cast<int>(FactoryPresetLibrary::kFactoryPresets.size()));

    for (const auto& preset : FactoryPresetLibrary::kFactoryPresets)
        names.add(juce::String(preset.name));

    return names;
}

bool SpatialEchoAudioProcessor::factoryPresetExists(const juce::String& name) const
{
    return findFactoryPresetIndexByName(name) >= 0;
}

void SpatialEchoAudioProcessor::loadFactoryPreset(const juce::String& name)
{
    ScopedSpatialParameterBatch spatialBatch(*this);
    const auto presetName = name.trim();
    if (presetName.isEmpty() || !defaultStateSnapshot.isValid())
        return;

    const int presetIndex = findFactoryPresetIndexByName(presetName);
    if (presetIndex < 0)
        return;

    const auto& definition = FactoryPresetLibrary::kFactoryPresets[static_cast<size_t>(presetIndex)];

    ScopedProcessorSuspend scopedSuspend(*this);
    loadDefaultPresetInternal(false);

    auto applyParameterValue = [this](const char* parameterId, float plainValue)
    {
        if (auto* parameter = treestate.getParameter(parameterId))
        {
            const float normalized = parameter->convertTo0to1(plainValue);
            parameter->setValueNotifyingHost(normalized);
        }
    };

    // Apply all parameters except tracked selector IDs first, then selectors last.
    for (size_t i = 0; i < FactoryPresetLibrary::kParameterIds.size(); ++i)
    {
        const auto* parameterId = FactoryPresetLibrary::kParameterIds[i];
        if (std::strcmp(parameterId, "PreampPreset") == 0 || std::strcmp(parameterId, "TapeFlavor") == 0)
            continue;
        applyParameterValue(parameterId, definition.values[i]);
    }

    for (size_t i = 0; i < FactoryPresetLibrary::kParameterIds.size(); ++i)
    {
        const auto* parameterId = FactoryPresetLibrary::kParameterIds[i];
        if (std::strcmp(parameterId, "PreampPreset") == 0 || std::strcmp(parameterId, "TapeFlavor") == 0)
            applyParameterValue(parameterId, definition.values[i]);
    }

    // Ensure factory presets are wet enough for immediate auditioning.
    float presetMix = 0.0f;
    bool hasMix = false;
    for (size_t i = 0; i < FactoryPresetLibrary::kParameterIds.size(); ++i)
    {
        if (std::strcmp(FactoryPresetLibrary::kParameterIds[i], "Mix") == 0)
        {
            presetMix = definition.values[i];
            hasMix = true;
            break;
        }
    }
    if (hasMix)
    {
        const float mixFloor = getFactoryPresetMixFloor(definition.category);
        if (presetMix < mixFloor)
            applyParameterValue("Mix", mixFloor);
    }

    syncPresetTrackingFromState();
}

//==============================================================================
juce::StringArray SpatialEchoAudioProcessor::getUserPresetNames() const
{
    juce::StringArray names;

    // The on-disk user-preset library is the only user-preset store; host
    // project state does not duplicate it.
    auto presetDir = getUserPresetDirectory();
    if (presetDir.exists())
    {
        juce::Array<juce::File> files;
        presetDir.findChildFiles(files, juce::File::findFiles, false, "*" + juce::String(kUserPresetFileExtension));
        for (const auto& file : files)
        {
            juce::String diskName = file.getFileNameWithoutExtension();
            if (auto xml = juce::parseXML(file))
            {
                if (xml->hasTagName(kUserPresetWrapperTag))
                {
                    const auto wrappedName = xml->getStringAttribute("name").trim();
                    if (wrappedName.isNotEmpty())
                        diskName = wrappedName;
                }
            }

            if (diskName.isNotEmpty())
                names.addIfNotAlreadyThere(diskName);
        }
    }

    names.sort(true);
    return names;
}

juce::File SpatialEchoAudioProcessor::getUserPresetDirectoryOnDisk() const
{
    return getUserPresetDirectory();
}

bool SpatialEchoAudioProcessor::userPresetExists(const juce::String& name) const
{
    const auto presetName = sanitizePresetName(name);
    if (presetName.isEmpty())
        return false;

    auto names = getUserPresetNames();
    return names.contains(presetName);
}

bool SpatialEchoAudioProcessor::saveUserPreset(const juce::String& name)
{
    const auto presetName = sanitizePresetName(name);
    if (presetName.isEmpty())
        return false;

    // The on-disk user-preset library is the only store; host project state
    // carries no preset copies.
    auto stateCopy = createStateSnapshot();
    auto presetFile = getPresetFileForName(presetName);
    return presetFile != juce::File() && writePresetFile(stateCopy, presetFile, presetName);
}

void SpatialEchoAudioProcessor::loadUserPreset(const juce::String& name)
{
    ScopedSpatialParameterBatch spatialBatch(*this);
    const auto presetName = sanitizePresetName(name);
    if (presetName.isEmpty())
        return;

    // The on-disk user-preset library is the only user-preset store.
    const auto diskPresetFile = getPresetFileForName(presetName);
    juce::ValueTree newState;
    if (diskPresetFile.existsAsFile())
        newState = readPresetStateFromFile(diskPresetFile, treestate.state.getType());

    if (!newState.isValid())
        return;

    {
        ScopedProcessorSuspend scopedSuspend(*this);

        // Reset to defaults first so parameters missing from older preset
        // files land on their defaults, then layer the preset's values on
        // top - all through the undoable parameter path. replaceState would
        // clear the UndoManager history and make the load unrecoverable
        // (PresetLoadUndoRecovery test); in-place application also preserves
        // editor-owned root properties and ignores the preset's own.
        loadDefaultPresetInternal(false);
        applyPresetParameterValuesFromState(treestate, newState);

        syncPresetTrackingFromState();
        changesApplied.clear();
        reloadDefaultHrtf();
    }
}

bool SpatialEchoAudioProcessor::deleteUserPreset(const juce::String& name)
{
    const auto presetName = sanitizePresetName(name);
    if (presetName.isEmpty())
        return false;

    auto presetFile = getPresetFileForName(presetName);
    if (!presetFile.existsAsFile())
        return false;

    return presetFile.deleteFile();
}

bool SpatialEchoAudioProcessor::renameUserPreset(const juce::String& oldName, const juce::String& newName)
{
    const auto oldPresetName = sanitizePresetName(oldName);
    const auto newPresetName = sanitizePresetName(newName);
    if (oldPresetName.isEmpty() || newPresetName.isEmpty())
        return false;
    if (oldPresetName == newPresetName)
        return false;
    if (userPresetExists(newPresetName))
        return false;

    const auto oldFile = getPresetFileForName(oldPresetName);
    const auto newFile = getPresetFileForName(newPresetName);
    if (oldFile.existsAsFile() && newFile != juce::File())
    {
        auto state = readPresetStateFromFile(oldFile, treestate.state.getType());
        if (state.isValid())
        {
            auto stateCopy = state.createCopy();
            auto existingPresets = stateCopy.getChildWithName("Presets");
            if (existingPresets.isValid())
                stateCopy.removeChild(existingPresets, nullptr);

            if (!writePresetFile(stateCopy, newFile, newPresetName))
                return false;
            return oldFile.deleteFile();
        }
    }

    return false;
}

juce::String SpatialEchoAudioProcessor::getPresetNameFromFile(const juce::File& file) const
{
    if (!file.existsAsFile())
        return {};
    return sanitizePresetName(readPresetNameFromFile(file));
}

bool SpatialEchoAudioProcessor::importUserPresetFromFile(const juce::File& file, juce::String* outName)
{
    ScopedSpatialParameterBatch spatialBatch(*this);
    if (!file.existsAsFile())
        return false;

    auto newState = readPresetStateFromFile(file, treestate.state.getType());
    if (!newState.isValid())
        return false;

    auto presetName = sanitizePresetName(readPresetNameFromFile(file));
    if (presetName.isEmpty())
        presetName = "Imported";

    // Commit the imported file before changing the current sound. Failed
    // storage must leave both the current state and the caller's identity alone.
    auto embeddedPresets = newState.getChildWithName("Presets");
    if (embeddedPresets.isValid())
        newState.removeChild(embeddedPresets, nullptr);
    migrateMissingExtensionParameters(newState);
    removeExternalHrtfPath(newState);
    newState.setProperty("stateSchemaVersion", 1, nullptr);
    if (!writePresetFile(newState, getPresetFileForName(presetName), presetName))
        return false;

    // Preserve scope visibility across import (same rationale as loadUserPreset).
    const auto savedScopeVisible = treestate.state.getProperty("scopeVisible");
    if (!savedScopeVisible.isVoid())
        newState.setProperty("scopeVisible", savedScopeVisible, nullptr);

    {
        ScopedProcessorSuspend scopedSuspend(*this);
        treestate.replaceState(newState);
        syncPresetTrackingFromState();
        changesApplied.clear();
        reloadDefaultHrtf();
    }

    if (outName != nullptr)
        *outName = presetName;

    return true;
}

bool SpatialEchoAudioProcessor::exportUserPresetToFile(const juce::File& file, const juce::String& name)
{
    if (file == juce::File())
        return false;

    auto stateCopy = createStateSnapshot();
    return writePresetFile(stateCopy, file, name);
}

void SpatialEchoAudioProcessor::loadDefaultPreset()
{
    loadDefaultPresetInternal(true);
}

void SpatialEchoAudioProcessor::loadDefaultPresetInternal(bool reloadHrtf)
{
    ScopedSpatialParameterBatch spatialBatch(*this);
    if (!defaultStateSnapshot.isValid())
        return;

    ScopedProcessorSuspend scopedSuspend(*this);

    // Restore every parameter to its captured default through the host-
    // notifying path instead of replaceState: replaceState clears the
    // UndoManager history, so an accidental preset load could not be undone
    // (PresetLoadUndoRecovery test). In-place
    // application also leaves editor-owned root properties (scopeVisible,
    // activePresetName/Type) untouched.
    applyPresetParameterValuesFromState(treestate, defaultStateSnapshot);

    syncPresetTrackingFromState();
    changesApplied.clear();

    if (reloadHrtf)
        reloadDefaultHrtf();
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SpatialEchoAudioProcessor();
}

float SpatialEchoAudioProcessor::freeRunningBaseDelayMs(float repeatRate) noexcept
{
    return 175.0f - (shapeRepeatRate(repeatRate) * 105.0f);
}

AudioProcessorValueTreeState::ParameterLayout SpatialEchoAudioProcessor::params()
{
    juce::AudioProcessorValueTreeState::ParameterLayout parameters;

    // ========== VALUE-TO-STRING FORMATTERS (Hybrid: technical + semantic) ==========

    // Percentage display (0-100%)
    auto percentFormat = [](float v, int) { return juce::String(juce::roundToInt(v * 100.0f)) + "%"; };

    // Output gain display (dB, 0.5 = 0 dB, range -12 to +12)
    auto outputGainDbFormat = [](float v, int)
    {
        float db = (v - 0.5f) * 24.0f;
        return (db >= 0.0f ? "+" : "") + juce::String(db, 1) + " dB";
    };

    // Branch level trims (dB)
    auto branchLevelDbFormat = [](float v, int) { return (v >= 0.0f ? "+" : "") + juce::String(v, 1) + " dB"; };

    // dB display for EQ (-12 to +12 dB centered)
    auto eqDbFormat = [](float v, int)
    {
        float db = (v - 0.5f) * 24.0f;  // 0-1 maps to -12 to +12 dB
        return (db >= 0.0f ? "+" : "") + juce::String(db, 1) + " dB";
    };

    // Input Level display (dB, 0.5 = 0 dB)
    auto inputLevelDbFormat = [](float v, int)
    {
        float db = mapInputLevelToDb(v);
        return (db >= 0.0f ? "+" : "") + juce::String(db, 1) + " dB";
    };

    // Repeat Rate: show the actual free-running base delay time used by the DSP.
    auto rateFormat = [](float v, int) { return juce::String(juce::roundToInt(freeRunningBaseDelayMs(v))) + " ms"; };

    // Intensity/Feedback (percentage with self-osc warning)
    auto intensityFormat = [](float v, int)
    {
        if (v > 0.95f)
            return juce::String("Self-Osc!");
        return juce::String(juce::roundToInt(v * 100.0f)) + "%";
    };

    // Hz display
    auto hzFormat = [](float v, int) { return juce::String(v, 2) + " Hz"; };

    // Tape Drive: plain percent. Zone words would collide with the TapeFlavor
    // vocabulary and the longest would not fit the value box (the UI never
    // truncates with an ellipsis).
    auto driveFormat = [](float v, int) { return juce::String(juce::roundToInt(v * 100.0f)) + "%"; };

    // Reverb Size (Small to Large)
    auto sizeFormat = [](float v, int)
    {
        if (v < 0.3f)
            return juce::String("Small");
        if (v < 0.7f)
            return juce::String("Medium");
        return juce::String("Large");
    };

    // Reverb Decay (actual T60 time)
    auto decayFormat = [](float v, int)
    {
        constexpr float minT60 = 0.4f;
        constexpr float maxT60 = 8.0f;
        float t60 = minT60 * std::pow(maxT60 / minT60, v);  // 0.4s to 8.0s
        return juce::String(t60, 1) + "s";
    };

    // Pre-delay (ms)
    auto preDelayFormat = [](float v, int)
    {
        float ms = v * 200.0f;
        return juce::String(juce::roundToInt(ms)) + " ms";
    };

    auto duckFormat = [](float v, int)
    {
        if (v < 0.02f)
            return juce::String("Off");
        if (v < 0.35f)
            return juce::String("Subtle");
        if (v < 0.70f)
            return juce::String("Medium");
        return juce::String("Deep");
    };

    // Spatial source width (focused to wide).
    auto widthFormat = [](float v, int)
    {
        if (v < 0.2f)
            return juce::String("Focused");
        if (v < 0.6f)
            return juce::String("Natural");
        return juce::String("Wide");
    };

    // Degrees display
    auto degFormat = [](float v, int)
    { return juce::String(juce::roundToInt(v)) + juce::String::charToString(0x00B0); };

    // Meters display
    auto meterFormat = [](float v, int) { return juce::String(v, 1) + " m"; };

    // ========== INPUT GROUP ==========
    auto inputgroup = std::make_unique<juce::AudioProcessorParameterGroup>("Input", "INPUT", "|");

    inputgroup->addChild(
        std::make_unique<AudioParameterChoice>(juce::ParameterID{"PreampPreset", 1}, "Preamp Preset",
                                               juce::StringArray{"Clean", "Warm", "Bright", "Drive", "Lo-Fi", "Custom"},
                                               0, juce::AudioParameterChoiceAttributes().withAutomatable(false)));
    inputgroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{"InputLevel", 1}, "Input Level", juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f,
        juce::AudioParameterFloatAttributes().withAutomatable(false).withStringFromValueFunction(inputLevelDbFormat)));
    inputgroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{"Bass", 1}, "Bass", juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f,
        juce::AudioParameterFloatAttributes().withAutomatable(false).withStringFromValueFunction(eqDbFormat)));
    inputgroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{"Treble", 1}, "Treble", juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f,
        juce::AudioParameterFloatAttributes().withAutomatable(false).withStringFromValueFunction(eqDbFormat)));

    parameters.add(std::move(inputgroup));

    auto delaygroup = std::make_unique<juce::AudioProcessorParameterGroup>("Delay", "DELAY", "|");

    delaygroup->addChild(std::make_unique<AudioParameterChoice>(
        juce::ParameterID{"DelaySetting", 1}, "Echo Mode",
        StringArray("H1 Echo", "H2 Echo", "H3 Echo", "H2+H3 Echo", "H1 Echo+Reverb", "H2 Echo+Reverb", "H3 Echo+Reverb",
                    "H1+H2 Echo+Reverb", "H2+H3 Echo+Reverb", "H1+H3 Echo+Reverb", "All Heads Echo+Reverb",
                    "Reverb Only"),
        0));
    delaygroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{"RepeatRate", 1}, "Repeat Rate", juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(rateFormat)));

    // Tempo sync controls - syncs delay time to host DAW tempo
    delaygroup->addChild(
        std::make_unique<AudioParameterBool>(juce::ParameterID{"TempoSync", 1}, "Tempo Sync", false,
                                             juce::AudioParameterBoolAttributes().withAutomatable(false)));

    // Per-head tempo sync subdivisions (cascade defaults: 1/4 -> 1/8 -> 1/16)
    // Straight, triplet and dotted divisions; one label list shared with the
    // motion DIV parameter and the editor's division menus.
    StringArray noteDivisions(SpatialIDs::motion::kDivisionChoices, SpatialIDs::motion::kDivisionCount);
    delaygroup->addChild(std::make_unique<AudioParameterChoice>(
        juce::ParameterID{"Head1NoteDivision", 1}, "Head 1 Division", noteDivisions, 2,
        juce::AudioParameterChoiceAttributes().withAutomatable(false)));  // Default 1/4
    delaygroup->addChild(std::make_unique<AudioParameterChoice>(
        juce::ParameterID{"Head2NoteDivision", 1}, "Head 2 Division", noteDivisions, 3,
        juce::AudioParameterChoiceAttributes().withAutomatable(false)));  // Default 1/8
    delaygroup->addChild(std::make_unique<AudioParameterChoice>(
        juce::ParameterID{"Head3NoteDivision", 1}, "Head 3 Division", noteDivisions, 4,
        juce::AudioParameterChoiceAttributes().withAutomatable(false)));  // Default 1/16

    // Head ratio preset: selects the timing relationship between the heads
    // (ratios defined in HeadRatioConstants.h). 0=Classic (1:1.9:2.75, default),
    // 1=Idealized (1:2:3), 2=Golden (1:1.618:2.618), 3=Linear (1:1.5:2)
    delaygroup->addChild(
        std::make_unique<AudioParameterChoice>(juce::ParameterID{"HeadRatioPreset", 1}, "Head Ratio Preset",
                                               juce::StringArray{"Classic (1:1.9:2.75)", "Idealized (1:2:3)",
                                                                 "Golden Ratio (1:1.62:2.62)", "Linear (1:1.5:2)"},
                                               0, juce::AudioParameterChoiceAttributes().withAutomatable(false)));

    // Intensity: 0.4 default = moderate feedback, safely below self-oscillation.
    // Display name "Feedback" (ID stays "Intensity" so sessions/automation keep
    // resolving): the name says what the knob actually does.
    delaygroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{"Intensity", 1}, "Feedback", juce::NormalisableRange<float>(0.0f, 1.0f), 0.4f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(intensityFormat)));
    // Dub switch: true = Normal (input feeds delay), false = Dub (input muted,
    // feedback continues). ID stays "EchoSend" for session compatibility.
    delaygroup->addChild(std::make_unique<AudioParameterBool>(
        juce::ParameterID{"EchoSend", 1}, "Dub Switch", true,
        juce::AudioParameterBoolAttributes().withAutomatable(false).withStringFromValueFunction(
            [](bool inputSendEnabled, int) { return juce::String(inputSendEnabled ? "Normal" : "Dub"); })));
    // Power switch: ON = delay running, OFF = triggers kill (clears buffer)
    delaygroup->addChild(std::make_unique<AudioParameterBool>(juce::ParameterID{"Power", 1}, "Power", true));

    // Auto-panic watchdog: flushes the wet chain automatically on genuine
    // blowups only (NaN/Inf or extreme sustained runaway). Intended
    // self-oscillation never trips it - see runAutoPanicWatchdog. Global
    // safety preference, so like OutputProtection it is deliberately NOT in
    // the factory-preset parameter list. Default OFF: the watchdog is opt-in
    // via the SPATIAL ECHO menu.
    delaygroup->addChild(
        std::make_unique<AudioParameterBool>(juce::ParameterID{"AutoPanic", 1}, "Auto-Panic", false,
                                             juce::AudioParameterBoolAttributes().withAutomatable(false)));

    // Stereo delay mode: Ping-Pong toggle (off = Normal, on = Ping-Pong)
    delaygroup->addChild(
        std::make_unique<AudioParameterBool>(juce::ParameterID{"PingPong", 1}, "Ping-Pong", false,
                                             juce::AudioParameterBoolAttributes().withAutomatable(false)));

    // Wow/flutter depth and rates are per-flavor internals selected by the
    // TapeFlavor parameter (see TapeFlavors.h), not host parameters.

    parameters.add(std::move(delaygroup));

    // Reverb Section - consolidated macro schema.
    // Seven continuous, automatable macros + engine choice; all engine-native
    // parameters (damping, cuts, diffusion, scatter, mod rate, early/late,
    // line count, spring stereo) are derived inside ReverbProcessor.
    auto reverbgroup = std::make_unique<juce::AudioProcessorParameterGroup>("Reverb", "REVERB", "|");

    auto toneFormat = [](float v, int)
    {
        const int direction = static_cast<int>(std::round((v - 0.5f) * 200.0f));
        if (direction == 0)
            return juce::String("Neutral");
        return juce::String(direction > 0 ? "Bright" : "Dark");
    };

    // Engine selection: Spring (mono tank + width stereoizer) or Cloud (lush)
    reverbgroup->addChild(std::make_unique<AudioParameterChoice>(
        juce::ParameterID{"ReverbEngine", 1}, "Engine", StringArray("Spring", "Cloud"), 0,
        juce::AudioParameterChoiceAttributes().withAutomatable(false)));
    // Size: room/tank size; also drives early/late balance internally
    reverbgroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{"ReverbSize", 1}, "Size", juce::NormalisableRange<float>(0.0f, 1.0f), 0.7085f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(sizeFormat)));
    // Decay: T60 decay time
    reverbgroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{"ReverbDecay", 1}, "Decay", juce::NormalisableRange<float>(0.0f, 1.0f), 0.6802f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(decayFormat)));
    // Tone: dark↔bright macro (drives HF damping + wet low/high cut)
    reverbgroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{"ReverbTone", 1}, "Tone", juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(toneFormat)));
    // Modulation depth (rates are fixed per engine in the smooth band)
    reverbgroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{"ReverbMod", 1}, "Reverb Modulation", juce::NormalisableRange<float>(0.0f, 1.0f), 0.08f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(percentFormat)));
    // Character: Spring = boing/tension (balance+scatter+diffusion);
    //            Cloud  = bloom/density (diffusion+early/late voicing)
    reverbgroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{"ReverbCharacter", 1}, "Character", juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(percentFormat)));
    // PreDelay: reverb onset delay (0-1 maps to 0-200ms)
    reverbgroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{"ReverbPreDelay", 1}, "Pre-Delay", juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(preDelayFormat)));
    // Width: 0 = mono (classic tank / folded Cloud) … 1 = wide
    reverbgroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{"ReverbWidth", 1}, "Width", juce::NormalisableRange<float>(0.0f, 1.0f), 0.4f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(percentFormat)));
    // Ducking: dry-input keyed attenuation of the audible reverb branch only
    reverbgroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{"ReverbDuckAmount", 1}, "Ducking", juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f,
        juce::AudioParameterFloatAttributes().withAutomatable(false).withStringFromValueFunction(duckFormat)));
    // Routing: Parallel (classic), Echo to Reverb, or Reverb to Echo.
    reverbgroup->addChild(std::make_unique<AudioParameterChoice>(
        juce::ParameterID{"ReverbRouting", 1}, "Routing",
        StringArray("Parallel", juce::String(juce::CharPointer_UTF8("Echo \u2192 Reverb")),
                    juce::String(juce::CharPointer_UTF8("Reverb \u2192 Echo"))),
        0, juce::AudioParameterChoiceAttributes().withAutomatable(false)));

    parameters.add(std::move(reverbgroup));

    auto mastergroup = std::make_unique<juce::AudioProcessorParameterGroup>("Master", "MASTER", "|");
    // Dry/wet mix. Typed text accepts "50%" or "50".
    mastergroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{"Mix", 1}, "Mix", juce::NormalisableRange<float>(0.0f, 1.0f), 0.37f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction(percentFormat)
            .withValueFromStringFunction([](const juce::String& text)
                                         { return juce::jlimit(0.0f, 1.0f, text.getFloatValue() * 0.01f); })));
    mastergroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{"EchoLevel", 1}, "Echo Level", juce::NormalisableRange<float>(-12.0f, 12.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(branchLevelDbFormat)));
    mastergroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{"ReverbLevel", 1}, "Reverb Level", juce::NormalisableRange<float>(-12.0f, 12.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(branchLevelDbFormat)));
    // Output gain: plain 0..1 shown as (v - 0.5) * 24 dB. Typed text accepts
    // "-3 dB" or "-3" and inverts that mapping.
    mastergroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{"OutputGain", 1}, "Output", juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction(outputGainDbFormat)
            .withValueFromStringFunction([](const juce::String& text)
                                         { return juce::jlimit(0.0f, 1.0f, text.getFloatValue() / 24.0f + 0.5f); })));
    mastergroup->addChild(
        std::make_unique<AudioParameterBool>(juce::ParameterID{"MixLock", 1}, "Mix Lock", false,
                                             juce::AudioParameterBoolAttributes().withAutomatable(false)));
    mastergroup->addChild(
        std::make_unique<AudioParameterBool>(juce::ParameterID{"OutputProtection", 1}, "Output Protection", true,
                                             juce::AudioParameterBoolAttributes().withAutomatable(false)));
    mastergroup->addChild(
        std::make_unique<AudioParameterBool>(juce::ParameterID{"FeedbackCompensation", 1}, "Feedback Limiter", true,
                                             juce::AudioParameterBoolAttributes().withAutomatable(false)));

    parameters.add(std::move(mastergroup));

    // Tape section: one flavor selector, one DRIVE knob, and a master noise
    // override. Saturation/bias/condition/noise/wow-flutter values remain
    // per-flavor internals resolved in TapeFlavors::resolve().
    auto tapegroup = std::make_unique<juce::AudioProcessorParameterGroup>("Tape", "TAPE", "|");

    // Drive: the single continuous tape control. Anchor 0.45 reproduces the
    // flavor's bundle exactly (WARM at 0.45 is the default sound).
    tapegroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{"TapeDrive", 1}, "Drive", juce::NormalisableRange<float>(0.0f, 1.0f), 0.45f,
        juce::AudioParameterFloatAttributes().withAutomatable(false).withStringFromValueFunction(driveFormat)));
    tapegroup->addChild(std::make_unique<AudioParameterChoice>(
        juce::ParameterID{"TapeFlavor", 1}, "Tape Flavor",
        juce::StringArray{"CLEAN", "WARM", "HOT", "CASSETTE", "WORN"}, 1,
        juce::AudioParameterChoiceAttributes().withAutomatable(false)));  // Default: WARM
    // v4 appends this post-v1 Tape setting after all prior v1-v3 parameters,
    // preserving AUv2 parameter ordering for existing Logic/GarageBand sessions.
    tapegroup->addChild(
        std::make_unique<AudioParameterBool>(juce::ParameterID{"TapeNoiseEnabled", 4}, "Tape Noise", true,
                                             juce::AudioParameterBoolAttributes().withAutomatable(false)));

    parameters.add(std::move(tapegroup));

    // Spatial processing parameters. The stable group and parameter IDs retain
    // their internal Binaural names for session compatibility.
    auto binauralgroup = std::make_unique<juce::AudioProcessorParameterGroup>("Spatial", "SPATIAL", "|");

    // Processing, Azimuth, Elevation, Distance are AUTOMATABLE primary controls.
    binauralgroup->addChild(std::make_unique<AudioParameterBool>(juce::ParameterID{SpatialIDs::binaural::kEnabled, 1},
                                                                 "Spatial Processing", false));
    // Azimuth: horizontal angle -180 to +180 degrees (0 = center/front)
    binauralgroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{SpatialIDs::binaural::kAzimuth, 1}, "Azimuth",
        juce::NormalisableRange<float>(-180.0f, 180.0f, 1.0f), 0.0f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(degFormat)));
    // Elevation: vertical angle -90 to +90 degrees (0 = ear level)
    binauralgroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{SpatialIDs::binaural::kElevation, 1}, "Elevation",
        juce::NormalisableRange<float>(-90.0f, 90.0f, 1.0f), 0.0f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(degFormat)));
    // Distance: 0.1 to 10 meters (1m = default reference distance)
    binauralgroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{SpatialIDs::binaural::kDistance, 1}, "Distance",
        juce::NormalisableRange<float>(0.1f, 10.0f, 0.01f), 1.0f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(meterFormat)));
    // Manual lock toggles (UI only, stored in presets)
    binauralgroup->addChild(
        std::make_unique<AudioParameterBool>(juce::ParameterID{SpatialIDs::binaural::kLockAzimuth, 1}, "Azimuth Lock",
                                             false, juce::AudioParameterBoolAttributes().withAutomatable(false)));
    binauralgroup->addChild(
        std::make_unique<AudioParameterBool>(juce::ParameterID{SpatialIDs::binaural::kLockDistance, 1}, "Distance Lock",
                                             false, juce::AudioParameterBoolAttributes().withAutomatable(false)));
    binauralgroup->addChild(std::make_unique<AudioParameterBool>(
        juce::ParameterID{SpatialIDs::binaural::kLockElevation, 1}, "Elevation Lock", false,
        juce::AudioParameterBoolAttributes().withAutomatable(false)));
    // Spatial Mix: 0-100% rendered amount - non-automatable (advanced)
    binauralgroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{SpatialIDs::binaural::kWetMix, 1}, "Spatial Mix",
        juce::NormalisableRange<float>(0.0f, 100.0f), 100.0f,
        juce::AudioParameterFloatAttributes().withAutomatable(false).withStringFromValueFunction(
            [](float v, int) { return juce::String(juce::roundToInt(v)) + "%"; })));
    // Crossover frequency: bass below this frequency bypasses HRTF processing (from reference)
    binauralgroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{SpatialIDs::binaural::kCrossover, 1}, "Spatial Crossover",
        juce::NormalisableRange<float>(20.0f, 500.0f, 1.0f), 150.0f,
        juce::AudioParameterFloatAttributes().withAutomatable(false).withStringFromValueFunction(
            [](float v, int) { return juce::String(juce::roundToInt(v)) + " Hz"; })));
    // HRTF Quality Mode: Performance=0, Balanced=1, Max=2
    binauralgroup->addChild(
        std::make_unique<AudioParameterChoice>(juce::ParameterID{SpatialIDs::binaural::kQuality, 1}, "Spatial Quality",
                                               juce::StringArray{"Performance", "Balanced", "Max"}, 2,
                                               juce::AudioParameterChoiceAttributes().withAutomatable(false)));
    // Source Width: focused source through natural to wide.
    binauralgroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{SpatialIDs::binaural::kWidth, 1}, "Source Width", juce::NormalisableRange<float>(0.0f, 1.0f),
        0.5f, juce::AudioParameterFloatAttributes().withStringFromValueFunction(widthFormat)));
    // Spatial Routing: where binaural processing is applied in the signal chain
    // 0 = After Effects (default, current behavior - entire output spatialized uniformly)
    // 1 = Before Effects (echoes and reverb tail inherit the spatial position)
    binauralgroup->addChild(
        std::make_unique<AudioParameterChoice>(juce::ParameterID{SpatialIDs::routing::kSpatialRouting, 1},
                                               "Spatial Routing", juce::StringArray{"After Effects", "Before Effects"},
                                               0, juce::AudioParameterChoiceAttributes().withAutomatable(false)));

    parameters.add(std::move(binauralgroup));

    // Per-tap spatial positioning parameters (one position per echo head)
    auto headPositionGroup = std::make_unique<juce::AudioProcessorParameterGroup>("HeadPosition", "HEAD POSITION", "|");
    // Tap Position Mode: controls whether echo taps follow global binaural position or have independent positions
    headPositionGroup->addChild(std::make_unique<AudioParameterChoice>(
        juce::ParameterID{SpatialIDs::taps::kPositionMode, 1}, "Head Position Mode",
        juce::StringArray{"Linked", "Independent"}, 0, juce::AudioParameterChoiceAttributes().withAutomatable(false)));

    // Defaults form a spread formation (front-left / front-right / behind) so
    // switching to per-tap never stacks the three markers on one point - these
    // values are inert in Linked mode.
    // H1 (Head 1 / primary tap) spatial position
    headPositionGroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{SpatialIDs::taps::kHead1Azimuth, 1}, "Head 1 Azimuth",
        juce::NormalisableRange<float>(-180.0f, 180.0f, 1.0f), -60.0f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(degFormat)));
    headPositionGroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{SpatialIDs::taps::kHead1Elevation, 1}, "Head 1 Elevation",
        juce::NormalisableRange<float>(-90.0f, 90.0f, 1.0f), 0.0f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(degFormat)));
    headPositionGroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{SpatialIDs::taps::kHead1Distance, 1}, "Head 1 Distance",
        juce::NormalisableRange<float>(0.1f, 10.0f, 0.01f), 1.0f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(meterFormat)));

    // H2 (Head 2 / secondary tap) spatial position
    headPositionGroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{SpatialIDs::taps::kHead2Azimuth, 1}, "Head 2 Azimuth",
        juce::NormalisableRange<float>(-180.0f, 180.0f, 1.0f), 60.0f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(degFormat)));
    headPositionGroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{SpatialIDs::taps::kHead2Elevation, 1}, "Head 2 Elevation",
        juce::NormalisableRange<float>(-90.0f, 90.0f, 1.0f), 0.0f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(degFormat)));
    headPositionGroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{SpatialIDs::taps::kHead2Distance, 1}, "Head 2 Distance",
        juce::NormalisableRange<float>(0.1f, 10.0f, 0.01f), 1.0f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(meterFormat)));

    // H3 (Head 3 / tertiary tap) spatial position
    headPositionGroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{SpatialIDs::taps::kHead3Azimuth, 1}, "Head 3 Azimuth",
        juce::NormalisableRange<float>(-180.0f, 180.0f, 1.0f), 180.0f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(degFormat)));
    headPositionGroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{SpatialIDs::taps::kHead3Elevation, 1}, "Head 3 Elevation",
        juce::NormalisableRange<float>(-90.0f, 90.0f, 1.0f), 0.0f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(degFormat)));
    headPositionGroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{SpatialIDs::taps::kHead3Distance, 1}, "Head 3 Distance",
        juce::NormalisableRange<float>(0.1f, 10.0f, 0.01f), 1.2f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(meterFormat)));

    parameters.add(std::move(headPositionGroup));

    auto spatialMotionGroup =
        std::make_unique<juce::AudioProcessorParameterGroup>("SpatialMotion", "SPATIAL MOTION", "|");

    // Pattern-first motion schema (8 new parameters)
    // Build choice lists from SpatialParamIDs constants
    juce::StringArray patternChoices;
    for (int i = 0; i < SpatialIDs::motion::kPatternCount; ++i)
        patternChoices.add(SpatialIDs::motion::kPatternChoices[i]);
    spatialMotionGroup->addChild(std::make_unique<AudioParameterChoice>(
        juce::ParameterID{SpatialIDs::motion::kPattern, 1}, SpatialIDs::motion::kPatternDisplay, patternChoices,
        SpatialIDs::motion::kPatternDefault));

    // Rate: 0.01..8 Hz, log-skewed with the knob centre at the geometric mean
    // (~0.28 Hz) per the SpatialParamIDs.h contract - most musical rates are slow.
    {
        juce::NormalisableRange<float> rateRange(SpatialIDs::motion::kRateMinHz, SpatialIDs::motion::kRateMaxHz, 0.01f);
        rateRange.setSkewForCentre(0.28f);
        spatialMotionGroup->addChild(std::make_unique<AudioParameterFloat>(
            juce::ParameterID{SpatialIDs::motion::kRate, 1}, SpatialIDs::motion::kRateDisplay, rateRange,
            SpatialIDs::motion::kRateDefaultHz,
            juce::AudioParameterFloatAttributes().withStringFromValueFunction(hzFormat)));
    }

    // Sync: tempo sync on/off
    spatialMotionGroup->addChild(std::make_unique<AudioParameterBool>(juce::ParameterID{SpatialIDs::motion::kSync, 1},
                                                                      SpatialIDs::motion::kSyncDisplay,
                                                                      SpatialIDs::motion::kSyncDefault));

    // Keep this 17-choice list fixed: hosts store automation as normalized
    // values, so adding choices would move every saved division. Longer
    // divisions live in the separate long-division parameters.
    spatialMotionGroup->addChild(std::make_unique<AudioParameterChoice>(
        juce::ParameterID{SpatialIDs::motion::kDivision, 1}, SpatialIDs::motion::kDivisionDisplay, noteDivisions,
        SpatialIDs::motion::kDivisionDefault));

    // Depth/Spread live on 0..100 ranges - the value already IS the percent
    // (percentFormat is for 0..1 params and would read 45 as 4500%).
    auto percent100Format = [](float v, int) { return juce::String(juce::roundToInt(v)) + "%"; };

    // Depth: 0..100%
    spatialMotionGroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{SpatialIDs::motion::kDepth, 1}, SpatialIDs::motion::kDepthDisplay,
        juce::NormalisableRange<float>(SpatialIDs::motion::kDepthMin, SpatialIDs::motion::kDepthMax, 0.1f),
        SpatialIDs::motion::kDepthDefault,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(percent100Format)));

    // Retrig: Free, Transport, Note, Random
    juce::StringArray retrigChoices;
    for (int i = 0; i < SpatialIDs::motion::kRetrigCount; ++i)
        retrigChoices.add(SpatialIDs::motion::kRetrigChoices[i]);
    spatialMotionGroup->addChild(std::make_unique<AudioParameterChoice>(
        juce::ParameterID{SpatialIDs::motion::kRetrig, 1}, SpatialIDs::motion::kRetrigDisplay, retrigChoices,
        SpatialIDs::motion::kRetrigDefault, juce::AudioParameterChoiceAttributes().withAutomatable(false)));

    // Choreo: Unison, Constellation, Cascade
    juce::StringArray choreoChoices;
    for (int i = 0; i < SpatialIDs::motion::kChoreoCount; ++i)
        choreoChoices.add(SpatialIDs::motion::kChoreoChoices[i]);
    spatialMotionGroup->addChild(std::make_unique<AudioParameterChoice>(
        juce::ParameterID{SpatialIDs::motion::kChoreo, 1}, SpatialIDs::motion::kChoreoDisplay, choreoChoices,
        SpatialIDs::motion::kChoreoDefault));

    // Spread: 0..100%
    spatialMotionGroup->addChild(std::make_unique<AudioParameterFloat>(
        juce::ParameterID{SpatialIDs::motion::kSpread, 1}, SpatialIDs::motion::kSpreadDisplay,
        juce::NormalisableRange<float>(SpatialIDs::motion::kSpreadMin, SpatialIDs::motion::kSpreadMax, 0.1f),
        SpatialIDs::motion::kSpreadDefault,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(percent100Format)));

    // Appended after every v1 motion parameter. Version hint 2 prevents the
    // AUv2 wrapper from reordering the v1 parameters when Logic rescans the plug-in.
    juce::StringArray longDivisionChoices;
    for (int i = 0; i < SpatialIDs::motion::kLongDivisionCount; ++i)
        longDivisionChoices.add(SpatialIDs::motion::kLongDivisionChoices[i]);
    spatialMotionGroup->addChild(std::make_unique<AudioParameterChoice>(
        juce::ParameterID{SpatialIDs::motion::kLongDivision, 2}, SpatialIDs::motion::kLongDivisionDisplay,
        longDivisionChoices, SpatialIDs::motion::kLongDivisionDefault));

    // Separate v3 extension keeps the five-choice SpatialLongDivision domain
    // frozen so existing host automation remains mapped to the same values.
    juce::StringArray longVariantDivisionChoices;
    for (int i = 0; i < SpatialIDs::motion::kLongVariantDivisionCount; ++i)
        longVariantDivisionChoices.add(SpatialIDs::motion::kLongVariantDivisionChoices[i]);
    spatialMotionGroup->addChild(std::make_unique<AudioParameterChoice>(
        juce::ParameterID{SpatialIDs::motion::kLongVariantDivision, 3}, SpatialIDs::motion::kLongVariantDivisionDisplay,
        longVariantDivisionChoices, SpatialIDs::motion::kLongVariantDivisionDefault));

    parameters.add(std::move(spatialMotionGroup));

    return parameters;
}

void SpatialEchoAudioProcessor::populateParameters()
{
    PreampPresetParam = static_cast<AudioParameterChoice*>(treestate.getParameter("PreampPreset"));
    PreampPresetValue = treestate.getRawParameterValue("PreampPreset");
    InputLevel = treestate.getRawParameterValue("InputLevel");
    Bass = treestate.getRawParameterValue("Bass");
    Treble = treestate.getRawParameterValue("Treble");

    DelaySetting = (static_cast<AudioParameterChoice*>(treestate.getParameter("DelaySetting")));
    PingPong = treestate.getRawParameterValue("PingPong");
    RepeatRate = treestate.getRawParameterValue("RepeatRate");
    Intensity = treestate.getRawParameterValue("Intensity");

    // Tempo sync parameters
    TempoSync = treestate.getRawParameterValue("TempoSync");
    Head1NoteDivision = static_cast<AudioParameterChoice*>(treestate.getParameter("Head1NoteDivision"));
    Head2NoteDivision = static_cast<AudioParameterChoice*>(treestate.getParameter("Head2NoteDivision"));
    Head3NoteDivision = static_cast<AudioParameterChoice*>(treestate.getParameter("Head3NoteDivision"));

    // Head ratio preset
    HeadRatioPreset = static_cast<AudioParameterChoice*>(treestate.getParameter("HeadRatioPreset"));

    EchoSend = treestate.getRawParameterValue("EchoSend");
    Panic = treestate.getRawParameterValue("Power");
    AutoPanic = treestate.getRawParameterValue("AutoPanic");

    // Reverb engine + macro schema
    ReverbEngineParam = static_cast<AudioParameterChoice*>(treestate.getParameter("ReverbEngine"));
    ReverbSize = treestate.getRawParameterValue("ReverbSize");
    ReverbDecay = treestate.getRawParameterValue("ReverbDecay");
    ReverbTone = treestate.getRawParameterValue("ReverbTone");
    ReverbMod = treestate.getRawParameterValue("ReverbMod");
    ReverbCharacter = treestate.getRawParameterValue("ReverbCharacter");
    ReverbPreDelay = treestate.getRawParameterValue("ReverbPreDelay");
    ReverbWidth = treestate.getRawParameterValue("ReverbWidth");
    ReverbDuckAmount = treestate.getRawParameterValue("ReverbDuckAmount");
    ReverbRouting = static_cast<AudioParameterChoice*>(treestate.getParameter("ReverbRouting"));

    Mix = treestate.getRawParameterValue("Mix");
    EchoLevel = treestate.getRawParameterValue("EchoLevel");
    ReverbLevel = treestate.getRawParameterValue("ReverbLevel");
    MixLock = treestate.getRawParameterValue("MixLock");
    OutputGain = treestate.getRawParameterValue("OutputGain");
    OutputProtection = treestate.getRawParameterValue("OutputProtection");
    FeedbackCompensation = treestate.getRawParameterValue("FeedbackCompensation");

    // Noise / wow-flutter are per-flavor internal values, not host parameters.

    // Binaural parameters
    BinauralEnabled = treestate.getRawParameterValue(SpatialIDs::binaural::kEnabled);
    BinauralAzimuth = treestate.getRawParameterValue(SpatialIDs::binaural::kAzimuth);
    BinauralElevation = treestate.getRawParameterValue(SpatialIDs::binaural::kElevation);
    BinauralDistance = treestate.getRawParameterValue(SpatialIDs::binaural::kDistance);
    BinauralWetMix = treestate.getRawParameterValue(SpatialIDs::binaural::kWetMix);
    BinauralCrossover = treestate.getRawParameterValue(SpatialIDs::binaural::kCrossover);
    BinauralQuality = static_cast<AudioParameterChoice*>(treestate.getParameter(SpatialIDs::binaural::kQuality));
    BinauralSourceWidth = treestate.getRawParameterValue(SpatialIDs::binaural::kWidth);
    SpatialRouting = static_cast<AudioParameterChoice*>(treestate.getParameter(SpatialIDs::routing::kSpatialRouting));

    // Pattern-first motion schema
    MotionPattern = static_cast<AudioParameterChoice*>(treestate.getParameter(SpatialIDs::motion::kPattern));
    MotionRate = treestate.getRawParameterValue(SpatialIDs::motion::kRate);
    MotionSync = treestate.getRawParameterValue(SpatialIDs::motion::kSync);
    MotionDivision = static_cast<AudioParameterChoice*>(treestate.getParameter(SpatialIDs::motion::kDivision));
    MotionLongDivision = static_cast<AudioParameterChoice*>(treestate.getParameter(SpatialIDs::motion::kLongDivision));
    MotionLongVariantDivision =
        static_cast<AudioParameterChoice*>(treestate.getParameter(SpatialIDs::motion::kLongVariantDivision));
    MotionDepth = treestate.getRawParameterValue(SpatialIDs::motion::kDepth);
    MotionRetrig = static_cast<AudioParameterChoice*>(treestate.getParameter(SpatialIDs::motion::kRetrig));
    MotionChoreo = static_cast<AudioParameterChoice*>(treestate.getParameter(SpatialIDs::motion::kChoreo));
    MotionSpread = treestate.getRawParameterValue(SpatialIDs::motion::kSpread);

    // Per-tap spatial positioning parameters
    TapPositionMode = static_cast<AudioParameterChoice*>(treestate.getParameter(SpatialIDs::taps::kPositionMode));
    Head1Azimuth = treestate.getRawParameterValue(SpatialIDs::taps::kHead1Azimuth);
    Head1Elevation = treestate.getRawParameterValue(SpatialIDs::taps::kHead1Elevation);
    Head1Distance = treestate.getRawParameterValue(SpatialIDs::taps::kHead1Distance);
    Head2Azimuth = treestate.getRawParameterValue(SpatialIDs::taps::kHead2Azimuth);
    Head2Elevation = treestate.getRawParameterValue(SpatialIDs::taps::kHead2Elevation);
    Head2Distance = treestate.getRawParameterValue(SpatialIDs::taps::kHead2Distance);
    Head3Azimuth = treestate.getRawParameterValue(SpatialIDs::taps::kHead3Azimuth);
    Head3Elevation = treestate.getRawParameterValue(SpatialIDs::taps::kHead3Elevation);
    Head3Distance = treestate.getRawParameterValue(SpatialIDs::taps::kHead3Distance);

    // Tape parameters
    TapeDrive = treestate.getRawParameterValue("TapeDrive");
    TapeNoiseEnabled = treestate.getRawParameterValue("TapeNoiseEnabled");
    TapeFlavorParam = static_cast<AudioParameterChoice*>(treestate.getParameter("TapeFlavor"));

    syncPresetTrackingFromState();
}

void SpatialEchoAudioProcessor::syncPresetTrackingFromState()
{
    // Preset trackers are stateful; refresh them after init/state-load so
    // updateParameters doesn't re-apply presets on the first slow update.
    preampPresetApplyInProgress = false;
    pendingHostParamUpdateMask.store(0u, std::memory_order_release);
    mixLockHeldValue = juce::jlimit(0.0f, 1.0f, (Mix != nullptr) ? Mix->load() : 0.0f);
    mixLockWasEngaged = (MixLock != nullptr && MixLock->load() > 0.5f);

    if (PreampPresetParam != nullptr)
        lastPreampPresetIndex = PreampPresetParam->getIndex();
    else
        lastPreampPresetIndex = -1;
    lastPreampParamsValid = false;

    // Tape flavor system: the resolved bundle is recomputed every slow update
    // in updateParameters(); refresh the caches here so a state load lands on
    // sane values before the first audio callback.
    {
        const int flavorIdx =
            (TapeFlavorParam != nullptr) ? TapeFlavorParam->getIndex() : static_cast<int>(TapeFlavor::Warm);
        const float driveKnob = (TapeDrive != nullptr) ? TapeDrive->load() : 0.45f;
        const TapeFlavors::Resolved tape = TapeFlavors::resolve(flavorIdx, driveKnob);
        tapePresetAsperityOverride = tape.asperityOverride;
        tapePresetDropoutOverride = tape.dropoutOverride;
        tapePresetScrapeScale = tape.scrapeScale;
        tapePresetLpfScale = tape.lpfScale;
    }
}

juce::ValueTree SpatialEchoAudioProcessor::createStateSnapshot()
{
    const juce::ScopedLock lock(stateSnapshotLock);
    auto state = treestate.copyState();

    const auto setSnapshotValue = [this, &state](const char* parameterId, float value)
    {
        auto child = state.getChildWithProperty("id", parameterId);
        if (child.isValid())
            if (const auto* parameter = treestate.getParameter(parameterId))
                child.setProperty("value", parameter->convertFrom0to1(parameter->convertTo0to1(value)), nullptr);
    };

    // Read the published selector before the queue mask: the audio callback
    // publishes preset child values before marking that selector as applied.
    // The non-audio lock prevents a deferred flush from consuming these values
    // between copyState and the overlay; audio publication remains lock-free.
    const int appliedPreampIndex = lastPreampPresetIndex.load(std::memory_order_acquire);
    const uint32_t pendingMask = pendingHostParamUpdateMask.load(std::memory_order_acquire);
    const auto overlayPending = [&](const char* id, uint32_t bit, const std::atomic<float>& value)
    {
        if ((pendingMask & bit) != 0u)
            setSnapshotValue(id, value.load(std::memory_order_relaxed));
    };
    overlayPending(SpatialIDs::binaural::kAzimuth, pendingBinauralAzimuthMask, pendingBinauralAzimuth);
    overlayPending(SpatialIDs::binaural::kElevation, pendingBinauralElevationMask, pendingBinauralElevation);
    overlayPending(SpatialIDs::binaural::kDistance, pendingBinauralDistanceMask, pendingBinauralDistance);
    if ((pendingMask & pendingPreampPresetMask) != 0u)
        setSnapshotValue("PreampPreset", static_cast<float>(pendingPreampPresetIndex.load(std::memory_order_relaxed)));

    const auto preampState = state.getChildWithProperty("id", "PreampPreset");
    const int preampIndex = static_cast<int>(preampState.getProperty("value", -1));
    const int presetCount = static_cast<int>(sizeof(kPreampPresets) / sizeof(kPreampPresets[0]));
    const bool pendingPreampValues =
        (pendingMask & (pendingInputLevelMask | pendingBassMask | pendingTrebleMask)) != 0u;
    if (preampIndex >= 0 && preampIndex < presetCount)
    {
        // Queued child values come only from a named preamp preset. Resolve
        // them from the captured selector, since concurrent automation may
        // already be publishing the next selector's different child values.
        if (preampIndex != appliedPreampIndex || pendingPreampValues)
        {
            const auto& preset = kPreampPresets[preampIndex];
            setSnapshotValue("InputLevel", preset.input);
            setSnapshotValue("Bass", preset.bass);
            setSnapshotValue("Treble", preset.treble);
        }
    }
    else
    {
        overlayPending("InputLevel", pendingInputLevelMask, pendingInputLevel);
        overlayPending("Bass", pendingBassMask, pendingBass);
        overlayPending("Treble", pendingTrebleMask, pendingTreble);
    }

    auto embeddedPresets = state.getChildWithName("Presets");
    if (embeddedPresets.isValid())
        state.removeChild(embeddedPresets, nullptr);
    state.setProperty("stateSchemaVersion", 1, nullptr);
    return state;
}

SpatialEchoAudioProcessor::ScopedSpatialParameterBatch::ScopedSpatialParameterBatch(SpatialEchoAudioProcessor& owner)
    : processor(owner)
    , lock(owner.spatialWriterLock)
{
    if (processor.spatialWriterDepth++ == 0)
        processor.spatialPositionGeneration.fetch_add(1, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

SpatialEchoAudioProcessor::ScopedSpatialParameterBatch::~ScopedSpatialParameterBatch()
{
    if (--processor.spatialWriterDepth == 0)
        processor.spatialPositionGeneration.fetch_add(1, std::memory_order_seq_cst);
}

SpatialEchoAudioProcessor::SpatialPositionState SpatialEchoAudioProcessor::readSpatialPositionsForAudio()
{
    static_assert(std::atomic<uint64_t>::is_always_lock_free, "Spatial generation must be lock-free");
    const auto before = spatialPositionGeneration.load(std::memory_order_seq_cst);
    if ((before & 1u) != 0)
        return lastCompleteSpatialPositions;

    SpatialPositionState candidate;
    auto read = [](const std::atomic<float>* p, float fallback) { return p != nullptr ? p->load() : fallback; };
    candidate.azimuth = read(BinauralAzimuth, 0.0f);
    candidate.elevation = read(BinauralElevation, 0.0f);
    candidate.distance = read(BinauralDistance, 1.0f);
    auto& taps = candidate.taps;
    taps.positionMode = TapPositionMode != nullptr ? TapPositionMode->getIndex() : 0;
    taps.h1Azimuth = read(Head1Azimuth, 0.0f);
    taps.h1Elevation = read(Head1Elevation, 0.0f);
    taps.h1Distance = juce::jlimit(0.1f, 10.0f, read(Head1Distance, 1.0f));
    taps.h2Azimuth = read(Head2Azimuth, 0.0f);
    taps.h2Elevation = read(Head2Elevation, 0.0f);
    taps.h2Distance = juce::jlimit(0.1f, 10.0f, read(Head2Distance, 1.0f));
    taps.h3Azimuth = read(Head3Azimuth, 0.0f);
    taps.h3Elevation = read(Head3Elevation, 0.0f);
    taps.h3Distance = juce::jlimit(0.1f, 10.0f, read(Head3Distance, 1.0f));
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (before == spatialPositionGeneration.load(std::memory_order_seq_cst))
        lastCompleteSpatialPositions = candidate;
    // A writer in flight costs at most one prior formation, never a retry or lock.
    return lastCompleteSpatialPositions;
}

void SpatialEchoAudioProcessor::updateParameters(int currentBlockSamples)
{
    const bool doSlowUpdate = !changesApplied.test_and_set();
    const double sr = (getSampleRate() > 1.0) ? getSampleRate() : (lastSampleRate > 1.0 ? lastSampleRate : 48000.0);
    const int blockSamples = juce::jmax(1, currentBlockSamples);

    if (manualSpatialActivityRequested.exchange(false, std::memory_order_acq_rel))
    {
        manualSpatialActivitySamplesRemaining =
            juce::jmax(1, static_cast<int>(std::round(kManualSpatialActivityWindowSec * sr)));
    }

    const auto loadFloatParam = [this](std::atomic<float>* liveValue, float fallback,
                                       const std::atomic<float>& pendingValue, uint32_t pendingMaskBit)
    {
        if ((pendingHostParamUpdateMask.load(std::memory_order_acquire) & pendingMaskBit) != 0u)
            return pendingValue.load(std::memory_order_relaxed);
        return (liveValue != nullptr) ? liveValue->load() : fallback;
    };

    applyDelaySettings();
    // EchoSend: 1.0 = ECHO mode (normal), 0.0 = NORMAL/dub mode (mute input to delay, feedback continues)
    // In reverb-only mode (mode 11), echoSend has no effect since delay is disabled anyway
    float echoSendValue = (*EchoSend > 0.5f) ? 1.0f : 0.0f;

    // ================================================================
    // Preamp Presets
    // ================================================================
    bool preampPresetAppliedThisUpdate = false;
    const int preampCustomIndex = 5;
    if (doSlowUpdate && PreampPresetParam != nullptr)
    {
        int currentPresetIdx = PreampPresetParam->getIndex();
        if (currentPresetIdx != lastPreampPresetIndex)
        {
            if (currentPresetIdx != preampCustomIndex)
            {
                const int presetCount = static_cast<int>(sizeof(kPreampPresets) / sizeof(kPreampPresets[0]));
                if (currentPresetIdx >= 0 && currentPresetIdx < presetCount)
                {
                    const auto& preset = kPreampPresets[currentPresetIdx];
                    bool applyPreset = true;
                    if (lastPreampPresetIndex < 0 && currentPresetIdx == 0)
                    {
                        const float eps = 1.0e-4f;
                        float currentInput =
                            loadFloatParam(InputLevel, preset.input, pendingInputLevel, pendingInputLevelMask);
                        float currentBass = loadFloatParam(Bass, preset.bass, pendingBass, pendingBassMask);
                        float currentTreble = loadFloatParam(Treble, preset.treble, pendingTreble, pendingTrebleMask);
                        const bool differsFromClean = (std::abs(currentInput - preset.input) > eps) ||
                                                      (std::abs(currentBass - preset.bass) > eps) ||
                                                      (std::abs(currentTreble - preset.treble) > eps);
                        if (differsFromClean)
                        {
                            const int maxIdx = std::max(0, PreampPresetParam->choices.size() - 1);
                            const int clamped = std::max(0, std::min(preampCustomIndex, maxIdx));
                            if (PreampPresetValue != nullptr)
                                queuePendingChoiceHostUpdate(pendingPreampPresetIndex, pendingPreampPresetMask,
                                                             clamped);
                            currentPresetIdx = clamped;
                            lastPreampPresetIndex = clamped;
                            applyPreset = false;
                        }
                    }
                    if (applyPreset)
                    {
                        preampPresetApplyInProgress = true;
                        if (InputLevel != nullptr)
                            queuePendingFloatHostUpdate(pendingInputLevel, pendingInputLevelMask, preset.input);
                        if (Bass != nullptr)
                            queuePendingFloatHostUpdate(pendingBass, pendingBassMask, preset.bass);
                        if (Treble != nullptr)
                            queuePendingFloatHostUpdate(pendingTreble, pendingTrebleMask, preset.treble);
                        preampPresetApplyInProgress = false;
                        preampPresetAppliedThisUpdate = true;
                    }
                    else
                    {
                        currentPresetIdx = preampCustomIndex;
                    }
                }
            }
            lastPreampPresetIndex = currentPresetIdx;
        }
    }

    // Design: Apply non-linear tapers to RepeatRate and Intensity before passing to DSP.
    // These shaped values are critical for the intended feel.
    float repeatRaw = (RepeatRate != nullptr) ? RepeatRate->load() : 0.5f;
    float repeatShaped = shapeRepeatRate(repeatRaw);

    // Tempo sync: calculate per-head synced delay times when enabled
    bool tempoSyncEnabled = (TempoSync != nullptr) ? (*TempoSync > 0.5f) : false;
    float head1SyncDelayMs = 0.0f;
    float head2SyncDelayMs = 0.0f;
    float head3SyncDelayMs = 0.0f;
    if (tempoSyncEnabled && Head1NoteDivision != nullptr && Head2NoteDivision != nullptr &&
        Head3NoteDivision != nullptr)
    {
        head1SyncDelayMs = calculateSyncedDelayMs(hostBpm, Head1NoteDivision->getIndex());
        head2SyncDelayMs = calculateSyncedDelayMs(hostBpm, Head2NoteDivision->getIndex());
        head3SyncDelayMs = calculateSyncedDelayMs(hostBpm, Head3NoteDivision->getIndex());
    }

    float intensityRaw = (Intensity != nullptr) ? Intensity->load() : 0.4f;
    const float intensityForDSP = mapFeedbackToLoopGain(intensityRaw);

    // Get reverb settings
    int reverbEngineIndex = (ReverbEngineParam != nullptr) ? ReverbEngineParam->getIndex() : 0;
    int reverbRoutingIndex = (ReverbRouting != nullptr) ? ReverbRouting->getIndex() : 0;

    // Get stereo delay mode (0=Normal, 1=Ping-Pong)
    int stereoModeIndex = (PingPong != nullptr && *PingPong > 0.5f) ? 1 : 0;

    // Get binaural parameters
    bool binauralOn = (BinauralEnabled != nullptr) ? (*BinauralEnabled > 0.5f) : false;
    const auto spatialPositions = readSpatialPositionsForAudio();
    // MIDI retains its independent, documented per-parameter override semantics.
    const auto pendingSpatial = pendingHostParamUpdateMask.load(std::memory_order_acquire);
    float binauralAz =
        (pendingSpatial & pendingBinauralAzimuthMask) != 0 ? pendingBinauralAzimuth.load() : spatialPositions.azimuth;
    float binauralEl = (pendingSpatial & pendingBinauralElevationMask) != 0 ? pendingBinauralElevation.load()
                                                                            : spatialPositions.elevation;
    float binauralDist = (pendingSpatial & pendingBinauralDistanceMask) != 0 ? pendingBinauralDistance.load()
                                                                             : spatialPositions.distance;
    float binauralWet = (BinauralWetMix != nullptr) ? juce::jlimit(0.0f, 100.0f, BinauralWetMix->load()) : 100.0f;
    float binauralXover = (BinauralCrossover != nullptr) ? BinauralCrossover->load() : 150.0f;
    int binauralQual = (BinauralQuality != nullptr) ? BinauralQuality->getIndex() : 2;
    float binauralWidth = (BinauralSourceWidth != nullptr) ? BinauralSourceWidth->load() : 0.5f;

    auto tapSpatialParams = spatialPositions.taps;  // motion modifies this block's copy

    const uint32_t spatialPendingMask =
        pendingHostParamUpdateMask.load(std::memory_order_acquire) &
        (pendingBinauralAzimuthMask | pendingBinauralElevationMask | pendingBinauralDistanceMask);
    if (!spatialSourceTrackingInitialized)
    {
        spatialSourceTrackingInitialized = true;
        lastObservedSpatialAzimuth = binauralAz;
        lastObservedSpatialElevation = binauralEl;
        lastObservedSpatialDistance = binauralDist;
    }
    else
    {
        constexpr float azimuthEpsilon = 0.001f;
        constexpr float elevationEpsilon = 0.001f;
        constexpr float distanceEpsilon = 0.0005f;

        const bool azimuthChanged = std::abs(binauralAz - lastObservedSpatialAzimuth) > azimuthEpsilon;
        const bool elevationChanged = std::abs(binauralEl - lastObservedSpatialElevation) > elevationEpsilon;
        const bool distanceChanged = std::abs(binauralDist - lastObservedSpatialDistance) > distanceEpsilon;
        const bool spatialChanged = azimuthChanged || elevationChanged || distanceChanged;

        if (spatialChanged)
        {
            if (spatialPendingMask != 0u)
            {
                spatialControlSource.store(SpatialControlSource::MIDI, std::memory_order_relaxed);
            }
            else
            {
                const bool manualActive = manualSpatialActivitySamplesRemaining > 0;
                spatialControlSource.store(manualActive ? SpatialControlSource::Manual
                                                        : SpatialControlSource::Automation,
                                           std::memory_order_relaxed);
            }
        }

        lastObservedSpatialAzimuth = binauralAz;
        lastObservedSpatialElevation = binauralEl;
        lastObservedSpatialDistance = binauralDist;
    }

    if (manualSpatialActivitySamplesRemaining > 0)
        manualSpatialActivitySamplesRemaining = juce::jmax(0, manualSpatialActivitySamplesRemaining - blockSamples);

    const double blockDurationSec = static_cast<double>(blockSamples) / sr;
    spatialMotionBlockDurationSecForUi.store(static_cast<float>(blockDurationSec), std::memory_order_relaxed);

    // Pattern-first motion schema
    const int motionPatternIdx = (MotionPattern != nullptr) ? MotionPattern->getIndex() : 0;
    const auto motionPattern = static_cast<SpatialIDs::motion::Pattern>(juce::jlimit(0, 5, motionPatternIdx));
    const bool spatialMotionEnabled = (motionPattern != SpatialIDs::motion::Pattern::Still);
    bool spatialMotionApplied = false;
    if (spatialMotionEnabled)
    {
        const bool motionSync = (MotionSync != nullptr) ? (MotionSync->load() > 0.5f) : false;
        const int motionRetrigIdx = (MotionRetrig != nullptr) ? juce::jlimit(0, 3, MotionRetrig->getIndex()) : 0;
        const int standardDivisionIndex =
            (MotionDivision != nullptr)
                ? juce::jlimit(0, SpatialIDs::motion::kDivisionCount - 1, MotionDivision->getIndex())
                : SpatialIDs::motion::kDivisionDefault;
        const int longDivisionIndex =
            (MotionLongDivision != nullptr)
                ? juce::jlimit(0, SpatialIDs::motion::kLongDivisionCount - 1, MotionLongDivision->getIndex())
                : SpatialIDs::motion::kLongDivisionDefault;
        const int longVariantDivisionIndex = (MotionLongVariantDivision != nullptr)
                                                 ? juce::jlimit(0, SpatialIDs::motion::kLongVariantDivisionCount - 1,
                                                                MotionLongVariantDivision->getIndex())
                                                 : SpatialIDs::motion::kLongVariantDivisionDefault;
        const int combinedDivisionIndex =
            SpatialIDs::motion::toDivisionMenuIndex(standardDivisionIndex, longDivisionIndex, longVariantDivisionIndex);

        // Rate: free-running Hz or tempo-synced
        float motionRateHz = (MotionRate != nullptr) ? juce::jlimit(0.01f, 8.0f, MotionRate->load())
                                                     : SpatialIDs::motion::kRateDefaultHz;
        if (motionSync && MotionDivision != nullptr)
        {
            const float beatsPerSecond = static_cast<float>(juce::jmax(1.0, hostBpm) / 60.0);
            motionRateHz = beatsPerSecond / getMotionNoteDivisionMultiplier(combinedDivisionIndex);
        }
        // Synced periods can legitimately be slower than the free-rate knob's
        // 0.01 Hz floor (for example 16/1 at very low tempos).
        motionRateHz = motionSync
                           ? juce::jlimit(0.0001f, SpatialIDs::motion::kSyncedRateMaxHz, motionRateHz)
                           : juce::jlimit(SpatialIDs::motion::kRateMinHz, SpatialIDs::motion::kRateMaxHz, motionRateHz);
        spatialMotionRateHzForUi.store(motionRateHz, std::memory_order_relaxed);

        // Depth: 0..100% -> 0..1.0 normalized
        const float depth01 =
            (MotionDepth != nullptr) ? juce::jlimit(0.0f, 100.0f, MotionDepth->load()) / 100.0f : 0.45f;
        const bool motionHasDepth = depth01 > 0.0001f;

        // Phase advancement: handle sync, retrig, and random offset
        double phaseCycles = spatialMotionPhaseCycles;
        const bool transportLocked = (motionRetrigIdx == 1) && hostIsPlaying;
        if (transportLocked)
        {
            if (motionSync && MotionDivision != nullptr)
            {
                const double cyclesPerQuarter =
                    1.0 / static_cast<double>(getMotionNoteDivisionMultiplier(combinedDivisionIndex));
                phaseCycles = hostPpqPosition * cyclesPerQuarter;
            }
            else
            {
                const double bpmSafe = juce::jmax(1.0, hostBpm);
                const double transportSeconds = hostPpqPosition * (60.0 / bpmSafe);
                phaseCycles = transportSeconds * static_cast<double>(motionRateHz);
            }

            spatialMotionPhaseCycles = phaseCycles;
            spatialMotionRandomPhasePrimed = false;
            spatialMotionRandomStep = -1;
        }
        else
        {
            const bool noteRetriggerReset = (motionRetrigIdx == 2) && spatialMotionNoteOnThisBlock;
            if (noteRetriggerReset)
                spatialMotionPhaseCycles = 0.0;

            if (!noteRetriggerReset)
                spatialMotionPhaseCycles += static_cast<double>(motionRateHz) * blockDurationSec;
            phaseCycles = spatialMotionPhaseCycles;

            if (motionRetrigIdx == 3)
            {
                const int cycleIndex = static_cast<int>(std::floor(spatialMotionPhaseCycles));
                if (!spatialMotionRandomPhasePrimed || cycleIndex != spatialMotionRandomStep ||
                    spatialMotionNoteOnThisBlock)
                {
                    spatialMotionRandomStep = cycleIndex;
                    const float randomNorm = 0.5f * (SpatialMotionLfo::hashSigned(cycleIndex, 0.9137f) + 1.0f);
                    spatialMotionRandomPhaseOffsetCycles = static_cast<double>(randomNorm);
                    spatialMotionRandomPhasePrimed = true;
                }
                phaseCycles += spatialMotionRandomPhaseOffsetCycles;
            }
        }
        spatialMotionPreviewPhaseCyclesForUi.store(phaseCycles, std::memory_order_relaxed);
        spatialMotionPreviewPatternIdForUi.store(motionPatternIdx, std::memory_order_relaxed);
        spatialMotionPreviewDepth01ForUi.store(depth01, std::memory_order_relaxed);

        // Trajectory evaluation: pure function of pattern, phase, and depth
        const spatialmotion::TrajectoryOffsets trajectory =
            spatialmotion::evaluateTrajectory(motionPattern, phaseCycles, depth01);
        const float modAzTarget = trajectory.azimuthDeg;
        const float modElTarget = trajectory.elevationDeg;
        const float modDistTarget = trajectory.distanceMeters;

        // Apply implicit smoothing (12ms) for smooth modulation
        float modAz = modAzTarget;
        float modEl = modElTarget;
        float modDist = modDistTarget;
        constexpr float kImplicitSmoothTimeSec = 0.012f;
        if (motionHasDepth)
        {
            if (!spatialMotionSmootherPrimed)
            {
                spatialMotionSmoothedAz = modAzTarget;
                spatialMotionSmoothedEl = modElTarget;
                spatialMotionSmoothedDist = modDistTarget;
                spatialMotionSmootherPrimed = true;
            }
            else
            {
                const double alpha =
                    std::exp(-blockDurationSec / juce::jmax(1.0e-5, static_cast<double>(kImplicitSmoothTimeSec)));
                const float blend = static_cast<float>(1.0 - alpha);
                // Azimuth is circular: smooth along the shortest angular path so
                // Orbit's +180 -> -180 seam doesn't sweep back through the front.
                const float azDelta = std::remainder(modAzTarget - spatialMotionSmoothedAz, 360.0f);
                spatialMotionSmoothedAz = std::remainder(spatialMotionSmoothedAz + azDelta * blend, 360.0f);
                spatialMotionSmoothedEl += (modElTarget - spatialMotionSmoothedEl) * blend;
                spatialMotionSmoothedDist += (modDistTarget - spatialMotionSmoothedDist) * blend;
            }

            modAz = spatialMotionSmoothedAz;
            modEl = spatialMotionSmoothedEl;
            modDist = spatialMotionSmoothedDist;
        }
        else
        {
            spatialMotionSmoothedAz = modAzTarget;
            spatialMotionSmoothedEl = modElTarget;
            spatialMotionSmoothedDist = modDistTarget;
            spatialMotionSmootherPrimed = false;
        }

        // Apply modulation additively to binaural base position. Azimuth wraps
        // (continuous rotation crosses the +/-180 seam); elevation/distance clamp.
        binauralAz = std::remainder(binauralAz + modAz, 360.0f);
        binauralEl = juce::jlimit(-90.0f, 90.0f, binauralEl + modEl);
        binauralDist = juce::jlimit(0.1f, 10.0f, binauralDist + modDist);

        spatialMotionModAzForUi.store(modAz, std::memory_order_relaxed);
        spatialMotionModElForUi.store(modEl, std::memory_order_relaxed);
        spatialMotionModDistForUi.store(modDist, std::memory_order_relaxed);
        spatialMotionApplied = motionHasDepth;
        if (spatialMotionApplied)
            spatialControlSource.store(SpatialControlSource::Motion, std::memory_order_relaxed);

        // Per-tap motion constellation (Independent position mode with motion enabled)
        if (tapSpatialParams.positionMode == 1)
        {
            // Read spread and choreography parameters with null guards
            const float spread = (MotionSpread != nullptr) ? juce::jlimit(0.0f, 100.0f, MotionSpread->load()) : 0.0f;
            const int choreoIdx = (MotionChoreo != nullptr) ? juce::jlimit(0, 2, MotionChoreo->getIndex()) : 0;

            // Compute per-tap trajectory modulation (same phase offsets, same pattern/depth as global, per-tap
            // smoothing)
            const MotionTapParams tapMotion{spread, choreoIdx};
            const float spreadNorm = spread / 100.0f;

            // Apply per-tap motion with independent smoothing
            for (int i = 0; i < 3; ++i)
            {
                const auto tap = static_cast<size_t>(i);
                const float offsetCycles = calculateTapPhaseOffset(i, tapMotion) * spreadNorm;

                // Evaluate per-tap trajectory with choreography phase offset
                const spatialmotion::TrajectoryOffsets tapTrajectory =
                    spatialmotion::evaluateTrajectory(motionPattern, phaseCycles + offsetCycles, depth01);
                const float tapAzTarget = tapTrajectory.azimuthDeg;
                const float tapElTarget = tapTrajectory.elevationDeg;
                const float tapDistTarget = tapTrajectory.distanceMeters;

                // Apply per-tap smoothing mirroring global path
                float modAz_i = tapAzTarget;
                float modEl_i = tapElTarget;
                float modDist_i = tapDistTarget;

                if (motionHasDepth)
                {
                    if (!tapMotionSmootherPrimed)
                    {
                        tapMotionSmoothedAz[tap] = tapAzTarget;
                        tapMotionSmoothedEl[tap] = tapElTarget;
                        tapMotionSmoothedDist[tap] = tapDistTarget;
                    }
                    else
                    {
                        const double alpha = std::exp(-blockDurationSec /
                                                      juce::jmax(1.0e-5, static_cast<double>(kImplicitSmoothTimeSec)));
                        const float blend = static_cast<float>(1.0 - alpha);
                        // Circular azimuth smoothing - see the global path above.
                        const float azDelta = std::remainder(tapAzTarget - tapMotionSmoothedAz[tap], 360.0f);
                        tapMotionSmoothedAz[tap] = std::remainder(tapMotionSmoothedAz[tap] + azDelta * blend, 360.0f);
                        tapMotionSmoothedEl[tap] += (tapElTarget - tapMotionSmoothedEl[tap]) * blend;
                        tapMotionSmoothedDist[tap] += (tapDistTarget - tapMotionSmoothedDist[tap]) * blend;
                    }

                    modAz_i = tapMotionSmoothedAz[tap];
                    modEl_i = tapMotionSmoothedEl[tap];
                    modDist_i = tapMotionSmoothedDist[tap];
                }
                else
                {
                    tapMotionSmoothedAz[tap] = tapAzTarget;
                    tapMotionSmoothedEl[tap] = tapElTarget;
                    tapMotionSmoothedDist[tap] = tapDistTarget;
                }

                // Apply modulation additively to per-tap positions
                if (i == 0)
                {
                    tapSpatialParams.h1Azimuth = std::remainder(tapSpatialParams.h1Azimuth + modAz_i, 360.0f);
                    tapSpatialParams.h1Elevation = juce::jlimit(-90.0f, 90.0f, tapSpatialParams.h1Elevation + modEl_i);
                    tapSpatialParams.h1Distance = juce::jlimit(0.1f, 10.0f, tapSpatialParams.h1Distance + modDist_i);
                }
                else if (i == 1)
                {
                    tapSpatialParams.h2Azimuth = std::remainder(tapSpatialParams.h2Azimuth + modAz_i, 360.0f);
                    tapSpatialParams.h2Elevation = juce::jlimit(-90.0f, 90.0f, tapSpatialParams.h2Elevation + modEl_i);
                    tapSpatialParams.h2Distance = juce::jlimit(0.1f, 10.0f, tapSpatialParams.h2Distance + modDist_i);
                }
                else
                {
                    tapSpatialParams.h3Azimuth = std::remainder(tapSpatialParams.h3Azimuth + modAz_i, 360.0f);
                    tapSpatialParams.h3Elevation = juce::jlimit(-90.0f, 90.0f, tapSpatialParams.h3Elevation + modEl_i);
                    tapSpatialParams.h3Distance = juce::jlimit(0.1f, 10.0f, tapSpatialParams.h3Distance + modDist_i);
                }
            }

            // Arm the smoother only AFTER all three taps have seeded their state.
            // Setting this inside the loop would make taps 1 and 2 smooth against
            // uninitialised history on their very first block.
            if (motionHasDepth)
                tapMotionSmootherPrimed = true;
        }
        else
        {
            // Reset per-tap smoother when not in Independent mode
            tapMotionSmootherPrimed = false;
        }
    }
    else
    {
        spatialMotionSmootherPrimed = false;
        spatialMotionRandomPhasePrimed = false;
        spatialMotionRandomStep = -1;
        spatialMotionRateHzForUi.store(0.0f, std::memory_order_relaxed);
        spatialMotionModAzForUi.store(0.0f, std::memory_order_relaxed);
        spatialMotionModElForUi.store(0.0f, std::memory_order_relaxed);
        spatialMotionModDistForUi.store(0.0f, std::memory_order_relaxed);
        tapMotionSmootherPrimed = false;
    }

    // ================================================================
    // Tape flavor system: Flavor + Drive choose the complete tape character.
    // Tape Noise only suppresses that character's generated noise injections.
    // ================================================================
    const int tapeFlavorIdx =
        (TapeFlavorParam != nullptr) ? TapeFlavorParam->getIndex() : static_cast<int>(TapeFlavor::Warm);
    const float tapeDriveKnob = (TapeDrive != nullptr) ? TapeDrive->load() : 0.45f;
    const TapeFlavors::Resolved tape = TapeFlavors::resolve(tapeFlavorIdx, tapeDriveKnob);

    const bool tapeNoiseEnabled = TapeNoiseEnabled == nullptr || TapeNoiseEnabled->load() > 0.5f;
    const bool noiseOn = tape.noiseOn && tapeNoiseEnabled;
    const float noiseAmt = tape.hissLevel;
    const float condition = tape.condition;
    const float wfDepth = tape.wfDepthParam;
    const float wowRateScale = tape.wowRateScale;
    const float flutterRateScale = tape.flutterRateScale;
    const float tapeDrv = tape.drive;
    const float tapeSat = tape.saturation;
    const float tapeBs = tape.bias;
    tapePresetAsperityOverride = tape.asperityOverride;
    tapePresetDropoutOverride = tape.dropoutOverride;
    tapePresetScrapeScale = tape.scrapeScale;
    tapePresetLpfScale = tape.lpfScale;
    const float tapeMakeupGainDb = TapeFlavors::getTapeRecordMakeupDb(tapeFlavorIdx, tapeDriveKnob);
    echomodel.setTapeRecordMakeupGainDb(tapeMakeupGainDb);

    // InputLevel: 0-1 knob maps to -60..+12 dB (0.5 = 0 dB)
    // Design: Input level at noon = unity, allows controlled drive into saturation
    float rawInputLevel = loadFloatParam(InputLevel, 0.5f, pendingInputLevel, pendingInputLevelMask);
    float inputLevelDb = mapInputLevelToDb(rawInputLevel);
    float inputLevelGain = juce::Decibels::decibelsToGain(inputLevelDb);
    float bass = loadFloatParam(Bass, 0.5f, pendingBass, pendingBassMask);
    float treble = loadFloatParam(Treble, 0.5f, pendingTreble, pendingTrebleMask);

    // If any preamp control is edited, switch to Custom for transparency.
    if (doSlowUpdate && PreampPresetParam != nullptr)
    {
        int currentPresetIdx = PreampPresetParam->getIndex();
        if (!preampPresetAppliedThisUpdate && !preampPresetApplyInProgress && currentPresetIdx != preampCustomIndex &&
            lastPreampParamsValid)
        {
            const float eps = 1.0e-4f;
            const bool changed = (std::abs(rawInputLevel - lastPreampInput) > eps) ||
                                 (std::abs(bass - lastPreampBass) > eps) || (std::abs(treble - lastPreampTreble) > eps);

            if (changed)
            {
                const int maxIdx = std::max(0, PreampPresetParam->choices.size() - 1);
                const int clamped = std::max(0, std::min(preampCustomIndex, maxIdx));
                if (PreampPresetValue != nullptr)
                    queuePendingChoiceHostUpdate(pendingPreampPresetIndex, pendingPreampPresetMask, clamped);
                currentPresetIdx = clamped;
                lastPreampPresetIndex = clamped;
            }
        }

        lastPreampInput = rawInputLevel;
        lastPreampBass = bass;
        lastPreampTreble = treble;
        lastPreampParamsValid = true;
    }

    // Reverb macro schema (engine-native params derive inside ReverbProcessor)
    ReverbMacroParams reverbMacros;
    reverbMacros.size = (ReverbSize != nullptr) ? ReverbSize->load() : 0.7085f;
    reverbMacros.decay = (ReverbDecay != nullptr) ? ReverbDecay->load() : 0.6802f;
    reverbMacros.tone = (ReverbTone != nullptr) ? ReverbTone->load() : 0.5f;
    reverbMacros.mod = (ReverbMod != nullptr) ? ReverbMod->load() : 0.08f;
    reverbMacros.character = (ReverbCharacter != nullptr) ? ReverbCharacter->load() : 0.5f;
    reverbMacros.preDelay = (ReverbPreDelay != nullptr) ? ReverbPreDelay->load() : 0.0f;
    reverbMacros.width = (ReverbWidth != nullptr) ? ReverbWidth->load() : 0.4f;
    float rvDuckAmount = (ReverbDuckAmount != nullptr) ? ReverbDuckAmount->load() : 0.0f;

    // Get head ratio preset (0=Classic 1:1.901:2.751 artistic, 1=Idealized 1:2:3
    // documented RE-201, 2=Golden phi, 3=Linear 1:1.5:2) - see HeadRatioConstants.h
    int headRatioPresetIndex = (HeadRatioPreset != nullptr) ? HeadRatioPreset->getIndex() : 0;
    float mixValue = (Mix != nullptr) ? Mix->load() : 0.0f;
    const float echoLevelDb = (EchoLevel != nullptr) ? EchoLevel->load() : 0.0f;
    const float reverbLevelDb = (ReverbLevel != nullptr) ? ReverbLevel->load() : 0.0f;
    const float echoLevelGain = juce::Decibels::decibelsToGain(echoLevelDb);
    const float reverbLevelGain = juce::Decibels::decibelsToGain(reverbLevelDb);
    const bool feedbackCompensationEnabled =
        (FeedbackCompensation != nullptr) ? (FeedbackCompensation->load() > 0.5f) : true;
    const bool mixLocked = (MixLock != nullptr && MixLock->load() > 0.5f);
    if (mixLocked)
    {
        if (!mixLockWasEngaged)
            mixLockHeldValue = juce::jlimit(0.0f, 1.0f, mixValue);
        mixValue = mixLockHeldValue;
    }
    else
    {
        mixLockHeldValue = juce::jlimit(0.0f, 1.0f, mixValue);
    }
    mixLockWasEngaged = mixLocked;

    int spatialRoutingIndex = (SpatialRouting != nullptr) ? SpatialRouting->getIndex() : 0;

    reverbMacros.engine = reverbEngineIndex;
    echomodel.UpdateParameters(bass, treble, intensityForDSP, repeatShaped, playheadstates, delayEnabled[*DelaySetting],
                               mixValue, reverbEnabled[*DelaySetting], inputLevelGain, echoSendValue, noiseOn, noiseAmt,
                               condition, wfDepth, wowRateScale, flutterRateScale, stereoModeIndex, tempoSyncEnabled,
                               head1SyncDelayMs, head2SyncDelayMs, head3SyncDelayMs, binauralOn, binauralAz, binauralEl,
                               binauralDist, binauralWet, binauralXover, binauralQual, tapeDrv, tapeSat, tapeBs,
                               headRatioPresetIndex, reverbRoutingIndex, tapePresetAsperityOverride,
                               tapePresetDropoutOverride, tapePresetScrapeScale, tapePresetLpfScale,
                               feedbackCompensationEnabled, spatialRoutingIndex, echoLevelGain, reverbLevelGain,
                               rvDuckAmount, reverbMacros, tapSpatialParams, intensityRaw);
    // Width applies to every voice so Independent-mode taps share the same
    // source character as the global render.
    if (auto* field = echomodel.getSpatialField())
        field->setSourceWidth(binauralWidth * 100.0f);
}

void SpatialEchoAudioProcessor::valueTreePropertyChanged(juce::ValueTree& treeWhosePropertyHasChanged,
                                                         const juce::Identifier& property)
{
    juce::ignoreUnused(treeWhosePropertyHasChanged, property);
    // Clear atomic flag so that parameters are saved to internal variables
    changesApplied.clear();
}

//==============================================================================
// Spatial control source tracking and MIDI input
//==============================================================================

void SpatialEchoAudioProcessor::noteSpatialManualInteraction()
{
    manualSpatialActivityRequested.store(true, std::memory_order_release);
    spatialControlSource.store(SpatialControlSource::Manual, std::memory_order_relaxed);
}

void SpatialEchoAudioProcessor::queuePendingFloatHostUpdate(std::atomic<float>& pendingValue, uint32_t maskBit,
                                                            float value)
{
    pendingValue.store(value, std::memory_order_relaxed);
    pendingHostParamUpdateMask.fetch_or(maskBit, std::memory_order_release);
}

void SpatialEchoAudioProcessor::queuePendingChoiceHostUpdate(std::atomic<int>& pendingIndex, uint32_t maskBit,
                                                             int index)
{
    pendingIndex.store(index, std::memory_order_relaxed);
    pendingHostParamUpdateMask.fetch_or(maskBit, std::memory_order_release);
}

void SpatialEchoAudioProcessor::applyHostFloatParameterUpdate(const char* paramId, float value)
{
    SpatialEchoParameterWrite::setFloatById(treestate, paramId, value);
}

void SpatialEchoAudioProcessor::applyHostChoiceParameterUpdate(const char* paramId, int index)
{
    SpatialEchoParameterWrite::setChoiceById(treestate, paramId, index);
}

// Standalone-only MIDI (see processActiveBlock): CC 21/22/23 queue spatial
// azimuth/elevation/distance writes, and any Note On arms the motion Note
// retrigger for this block.
void SpatialEchoAudioProcessor::processSpatialMidiCC(const juce::MidiBuffer& midiMessages)
{
    spatialMotionNoteOnThisBlock = false;

    for (const auto metadata : midiMessages)
    {
        const auto msg = metadata.getMessage();

        if (msg.isNoteOn())
            spatialMotionNoteOnThisBlock = true;

        if (msg.isController())
        {
            const int cc = msg.getControllerNumber();
            const int value = msg.getControllerValue();  // 0-127

            // Map CC value to parameter range
            if (cc == kMidiCCAzimuth && BinauralAzimuth != nullptr)
            {
                // CC 0-127 -> Azimuth -180 to +180
                const float azimuth = juce::jmap(static_cast<float>(value), 0.0f, 127.0f, -180.0f, 180.0f);
                queuePendingFloatHostUpdate(pendingBinauralAzimuth, pendingBinauralAzimuthMask, azimuth);
                spatialControlSource.store(SpatialControlSource::MIDI);
            }
            else if (cc == kMidiCCElevation && BinauralElevation != nullptr)
            {
                // CC 0-127 -> Elevation -90 to +90
                const float elevation = juce::jmap(static_cast<float>(value), 0.0f, 127.0f, -90.0f, 90.0f);
                queuePendingFloatHostUpdate(pendingBinauralElevation, pendingBinauralElevationMask, elevation);
                spatialControlSource.store(SpatialControlSource::MIDI);
            }
            else if (cc == kMidiCCDistance && BinauralDistance != nullptr)
            {
                // CC 0-127 -> Distance 0.1 to 10.0 meters
                const float distance = juce::jmap(static_cast<float>(value), 0.0f, 127.0f, 0.1f, 10.0f);
                queuePendingFloatHostUpdate(pendingBinauralDistance, pendingBinauralDistanceMask, distance);
                spatialControlSource.store(SpatialControlSource::MIDI);
            }
        }
    }
}

void SpatialEchoAudioProcessor::timerCallback()
{
    flushPendingHostParameterUpdates();
}

void SpatialEchoAudioProcessor::flushPendingHostParameterUpdates()
{
    const juce::ScopedLock lock(stateSnapshotLock);
    const uint32_t pendingMask = pendingHostParamUpdateMask.exchange(0u, std::memory_order_acq_rel);
    if (pendingMask == 0u)
        return;

    if ((pendingMask & pendingBinauralAzimuthMask) != 0u)
        applyHostFloatParameterUpdate(SpatialIDs::binaural::kAzimuth,
                                      pendingBinauralAzimuth.load(std::memory_order_relaxed));
    if ((pendingMask & pendingBinauralElevationMask) != 0u)
        applyHostFloatParameterUpdate(SpatialIDs::binaural::kElevation,
                                      pendingBinauralElevation.load(std::memory_order_relaxed));
    if ((pendingMask & pendingBinauralDistanceMask) != 0u)
        applyHostFloatParameterUpdate(SpatialIDs::binaural::kDistance,
                                      pendingBinauralDistance.load(std::memory_order_relaxed));

    if ((pendingMask & pendingPreampPresetMask) != 0u)
        applyHostChoiceParameterUpdate("PreampPreset", pendingPreampPresetIndex.load(std::memory_order_relaxed));
    if ((pendingMask & pendingInputLevelMask) != 0u)
        applyHostFloatParameterUpdate("InputLevel", pendingInputLevel.load(std::memory_order_relaxed));
    if ((pendingMask & pendingBassMask) != 0u)
        applyHostFloatParameterUpdate("Bass", pendingBass.load(std::memory_order_relaxed));
    if ((pendingMask & pendingTrebleMask) != 0u)
        applyHostFloatParameterUpdate("Treble", pendingTreble.load(std::memory_order_relaxed));
}

// Reloads the embedded HRTF set on every spatial voice. prepareToPlay and every
// whole-state load (session restore, Init, user-preset load and import) call
// it, so each restored state starts from freshly prepared HRIR convolvers.
void SpatialEchoAudioProcessor::reloadDefaultHrtf()
{
    if (auto* field = echomodel.getSpatialField())
    {
        ScopedProcessorSuspend suspend(*this);
        field->useDefaultHRTF();
    }
}

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
void SpatialEchoAudioProcessor::enableStageTimingCapture(bool enabled)
{
    EchoesModelTestAccess::enableStageTimingCapture(echomodel, enabled);
}

void SpatialEchoAudioProcessor::resetStageTimingCapture()
{
    EchoesModelTestAccess::resetStageTimingCapture(echomodel);
}

bool SpatialEchoAudioProcessor::getStageTimingSnapshot(EchoesModel::StageTimingSnapshot& out) const
{
    return EchoesModelTestAccess::getStageTimingSnapshot(echomodel, out);
}
#endif

void SpatialEchoAudioProcessor::applyDelaySettings()
{
    if (DelaySetting->getIndex() == 0)
    {
        playheadstates[0] = 1;
        playheadstates[1] = 0;
        playheadstates[2] = 0;
    }

    if (DelaySetting->getIndex() == 1)
    {
        playheadstates[0] = 0;
        playheadstates[1] = 1;
        playheadstates[2] = 0;
    }

    if (DelaySetting->getIndex() == 2)
    {
        playheadstates[0] = 0;
        playheadstates[1] = 0;
        playheadstates[2] = 1;
    }

    if (DelaySetting->getIndex() == 3)
    {
        playheadstates[0] = 0;
        playheadstates[1] = 1;
        playheadstates[2] = 1;
    }

    if (DelaySetting->getIndex() == 4)
    {
        playheadstates[0] = 1;
        playheadstates[1] = 0;
        playheadstates[2] = 0;
    }

    if (DelaySetting->getIndex() == 5)
    {
        playheadstates[0] = 0;
        playheadstates[1] = 1;
        playheadstates[2] = 0;
    }

    if (DelaySetting->getIndex() == 6)
    {
        playheadstates[0] = 0;
        playheadstates[1] = 0;
        playheadstates[2] = 1;
    }

    if (DelaySetting->getIndex() == 7)
    {
        playheadstates[0] = 1;
        playheadstates[1] = 1;
        playheadstates[2] = 0;
    }

    if (DelaySetting->getIndex() == 8)
    {
        playheadstates[0] = 0;
        playheadstates[1] = 1;
        playheadstates[2] = 1;
    }

    if (DelaySetting->getIndex() == 9)
    {
        playheadstates[0] = 1;
        playheadstates[1] = 0;
        playheadstates[2] = 1;
    }

    if (DelaySetting->getIndex() == 10)
    {
        playheadstates[0] = 1;
        playheadstates[1] = 1;
        playheadstates[2] = 1;
    }
}
