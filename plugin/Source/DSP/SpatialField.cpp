// SpatialField: Multi-voice spatial audio field routing through binaural voices.
// Realtime-safety: no allocation/locks in processBlock()

#include "SpatialField.h"
#include <algorithm>
#include <cstring>

namespace
{
bool isValidVoiceIndex(int voiceIdx) noexcept
{
    return voiceIdx >= 0 && voiceIdx < SpatialField::kMaxVoices;
}
}  // namespace

SpatialField::SpatialField()
    : sampleRate(0.0)
    , samplesPerBlock(512)
    , numChannels(2)
{
    for (size_t i = 0; i < voices.size(); ++i)
    {
        voices[i] = std::make_unique<BinauralProcessor>();
        // Only the global voice is summed by processBlock()
        voiceEnabled[i] = (i == static_cast<size_t>(kGlobalVoiceIndex));
    }
}

void SpatialField::prepare(double newSampleRate, int newSamplesPerBlock, int newNumChannels)
{
    // Validate inputs
    if (newSamplesPerBlock <= 0 || newSamplesPerBlock > 65536)
    {
        jassertfalse;
        return;
    }

    sampleRate = newSampleRate;
    samplesPerBlock = newSamplesPerBlock;
    numChannels = std::clamp(newNumChannels, 1, 2);

    // Initialize all voice processors
    for (auto& voice : voices)
    {
        if (voice)
        {
            voice->prepare(newSampleRate, newSamplesPerBlock, numChannels);
            // Every voice needs HRTF data, or it processes with an empty
            // database (silent output).
            if (!voice->isHrtfLoaded())
                voice->useDefaultHRTF();
            voice->reset();  // Ensure reset after prepare
        }
    }

    // Allocate summing buffer (numChannels x samplesPerBlock)
    const auto channelCount = static_cast<size_t>(numChannels);
    sumBuffer.resize(channelCount);
    for (auto& channel : sumBuffer)
        channel.assign(static_cast<size_t>(newSamplesPerBlock), 0.0f);

    // Allocate per-voice work buffers (prevent aliasing on multi-voice processing)
    for (auto& workBuffer : voiceWorkBuffers)
    {
        workBuffer.resize(channelCount);
        for (auto& channel : workBuffer)
            channel.assign(static_cast<size_t>(newSamplesPerBlock), 0.0f);
    }
}

void SpatialField::reset()
{
    for (auto& voice : voices)
        if (voice)
            voice->reset();
}

void SpatialField::resetVoice(int voiceIdx)
{
    if (isValidVoiceIndex(voiceIdx) && voices[static_cast<size_t>(voiceIdx)])
        voices[static_cast<size_t>(voiceIdx)]->reset();
}

void SpatialField::clearNaNLatchableState()
{
    for (auto& voice : voices)
        if (voice)
            voice->clearNaNLatchableState();
}

void SpatialField::processBlock(juce::AudioBuffer<float>& buffer)
{
    // samplesPerBlock/numChannels have non-zero defaults from the constructor,
    // so they do NOT prove prepare() ran. sumBuffer and voiceWorkBuffers are
    // empty until then, and every access below indexes them with operator[],
    // which is unchecked. Gate on the buffers themselves.
    if (!isPrepared())
        return;

    if (samplesPerBlock <= 0 || numChannels <= 0)
        return;

    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0 || numSamples > samplesPerBlock)
        return;

    // Validate channel consistency
    if (buffer.getNumChannels() != numChannels)
    {
        jassertfalse;
        return;
    }

    const auto sampleCount = static_cast<size_t>(numSamples);

    // Clear sum buffer
    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto& sum = sumBuffer[static_cast<size_t>(ch)];
        std::fill(sum.begin(), sum.begin() + numSamples, 0.0f);
    }

    // Process each enabled voice and accumulate
    // Each voice receives the ORIGINAL input (not modified by prior voices)
    for (size_t voiceIdx = 0; voiceIdx < voices.size(); ++voiceIdx)
    {
        if (voiceEnabled[voiceIdx] && voices[voiceIdx])
        {
            auto& workBuffer = voiceWorkBuffers[voiceIdx];

            // Copy original input to per-voice work buffer
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float* origInput = buffer.getReadPointer(ch);
                float* voiceWorkData = workBuffer[static_cast<size_t>(ch)].data();
                std::memcpy(voiceWorkData, origInput, sizeof(float) * sampleCount);
            }

            // Create array of pointers for JUCE buffer constructor
            float* channelPtrs[2] = {nullptr, nullptr};
            channelPtrs[0] = workBuffer[0].data();
            if (numChannels > 1)
                channelPtrs[1] = workBuffer[1].data();

            // Create a temporary JUCE buffer pointing to the work buffer
            juce::AudioBuffer<float> voiceBuffer(const_cast<float* const*>(channelPtrs), numChannels, numSamples);

            // Process this voice in isolation
            voices[voiceIdx]->processBlock(voiceBuffer);

            // Accumulate voice output to sum buffer
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float* voiceOut = voiceBuffer.getReadPointer(ch);
                float* sumOut = sumBuffer[static_cast<size_t>(ch)].data();
                for (int s = 0; s < numSamples; ++s)
                {
                    sumOut[s] += voiceOut[s];
                }
            }
        }
    }

    // Copy sum buffer back to output
    for (int ch = 0; ch < numChannels; ++ch)
    {
        std::memcpy(buffer.getWritePointer(ch), sumBuffer[static_cast<size_t>(ch)].data(), sizeof(float) * sampleCount);
    }
}

void SpatialField::setVoiceAzimuth(int voiceIdx, float azimuthDegrees)
{
    if (auto* voice = getVoiceProcessor(voiceIdx))
        voice->setAzimuth(azimuthDegrees);
}

void SpatialField::setVoiceElevation(int voiceIdx, float elevationDegrees)
{
    if (auto* voice = getVoiceProcessor(voiceIdx))
        voice->setElevation(elevationDegrees);
}

void SpatialField::setVoiceDistance(int voiceIdx, float distanceMeters)
{
    if (auto* voice = getVoiceProcessor(voiceIdx))
        voice->setDistance(distanceMeters);
}

void SpatialField::setEnabled(bool enabled)
{
    for (auto& voice : voices)
        if (voice)
            voice->setEnabled(enabled);
}

void SpatialField::setWetMix(float wetPercent)
{
    for (auto& voice : voices)
        if (voice)
            voice->setWetMix(wetPercent);
}

void SpatialField::setHRTFQuality(BinauralProcessor::HRTFQuality quality)
{
    for (auto& voice : voices)
        if (voice)
            voice->setHRTFQuality(quality);
}

void SpatialField::setDiffuseFieldEQEnabled(bool enabled)
{
    for (auto& voice : voices)
        if (voice)
            voice->setDiffuseFieldEQEnabled(enabled);
}

void SpatialField::setCrossoverFrequency(float freqHz)
{
    for (auto& voice : voices)
        if (voice)
            voice->setCrossoverFrequency(freqHz);
}

void SpatialField::setSourceWidth(float widthPercent)
{
    for (auto& voice : voices)
        if (voice)
            voice->setSourceWidth(widthPercent);
}

bool SpatialField::loadSOFAFile(const juce::File& sofaFile)
{
    for (auto& voice : voices)
    {
        if (!voice || !voice->loadSOFAFile(sofaFile))
        {
            useDefaultHRTF();
            return false;
        }
    }

    return true;
}

void SpatialField::useDefaultHRTF()
{
    for (auto& voice : voices)
        if (voice)
            voice->useDefaultHRTF();
}

BinauralProcessor* SpatialField::getVoiceProcessor(int voiceIdx)
{
    return isValidVoiceIndex(voiceIdx) ? voices[static_cast<size_t>(voiceIdx)].get() : nullptr;
}

const BinauralProcessor* SpatialField::getVoiceProcessor(int voiceIdx) const
{
    return isValidVoiceIndex(voiceIdx) ? voices[static_cast<size_t>(voiceIdx)].get() : nullptr;
}
