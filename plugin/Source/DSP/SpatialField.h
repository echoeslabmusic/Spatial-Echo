// SpatialField: Multi-voice spatial audio field routing through binaural voices.
// Realtime-safety: no allocation/locks in processBlock()

#pragma once

#include "BinauralProcessor.h"
#include <JuceHeader.h>
#include <memory>
#include <array>

/**
 * SpatialField - Multi-voice binaural spatial audio field
 *
 * Owns one BinauralProcessor per voice. processBlock() renders the enabled
 * voices (voice 0, the Linked/global renderer) and sums them to stereo; the
 * per-tap voices are driven directly through getVoiceProcessor() by
 * EchoesModel in Independent head-position mode.
 *
 * RT-safe: no allocations or locks in the audio path.
 */
class SpatialField
{
public:
    // Voice 0 owns the Linked/global renderer. Voices 1..3 are reserved for
    // H1/H2/H3 so changing Head Position Mode never reuses an active
    // convolution history for a different signal.
    static constexpr int kGlobalVoiceIndex = 0;
    static constexpr int kTapVoiceOffset = 1;
    static constexpr int kTapVoiceCount = 3;
    static constexpr int kMaxVoices = kTapVoiceOffset + kTapVoiceCount;

    SpatialField();
    ~SpatialField() = default;

    // Initialize field and voices
    void prepare(double sampleRate, int samplesPerBlock, int numChannels);
    void reset();
    void resetVoice(int voiceIdx);
    // Auto-panic flush: per-voice IIR state clear (see BinauralProcessor).
    void clearNaNLatchableState();

    // Process audio through all active voices and sum to output buffer
    // Caller provides stereo working buffer; we convolve and return summed result.
    void processBlock(juce::AudioBuffer<float>& buffer);

    // Per-voice position
    void setVoiceAzimuth(int voiceIdx, float azimuthDegrees);
    void setVoiceElevation(int voiceIdx, float elevationDegrees);
    void setVoiceDistance(int voiceIdx, float distanceMeters);

    // Global field parameters. Every voice must share the same renderer
    // configuration so switching between Linked and Independent routes does
    // not change timbre or silently disable taps.
    void setEnabled(bool enabled);
    void setWetMix(float wetPercent);
    void setHRTFQuality(BinauralProcessor::HRTFQuality quality);
    void setDiffuseFieldEQEnabled(bool enabled);
    void setCrossoverFrequency(float freqHz);
    void setSourceWidth(float widthPercent);

    // SOFA lifecycle is field-wide. A custom dataset is accepted only when
    // every voice loads it; failures restore the bundled HRTF on all voices.
    bool loadSOFAFile(const juce::File& sofaFile);
    void useDefaultHRTF();

    // Access individual voice processor (for advanced configuration)
    // Guaranteed non-null for valid voiceIdx (0 <= voiceIdx < kMaxVoices) after prepare().
    // Voice 0 is always initialized; voices 1+ may be disabled but are non-null.
    [[nodiscard]] BinauralProcessor* getVoiceProcessor(int voiceIdx);
    [[nodiscard]] const BinauralProcessor* getVoiceProcessor(int voiceIdx) const;

private:
    // True only once prepare() has sized the mixing/work buffers. RT-safe:
    // reads two vector sizes, no allocation. processBlock() indexes those
    // buffers with unchecked operator[], so it must not run before this holds.
    [[nodiscard]] bool isPrepared() const noexcept
    {
        return sumBuffer.size() == static_cast<size_t>(numChannels) && !sumBuffer.empty() &&
               voiceWorkBuffers[0].size() == static_cast<size_t>(numChannels);
    }

    // Binaural voice pool (pre-allocated). processBlock() renders only the
    // enabled voices: voice 0.
    std::array<std::unique_ptr<BinauralProcessor>, kMaxVoices> voices;
    std::array<bool, kMaxVoices> voiceEnabled;

    // Mixing buffer (summing stage)
    std::vector<std::vector<float>> sumBuffer;

    // Per-voice work buffers to isolate processing (prevent aliasing)
    std::vector<std::vector<float>> voiceWorkBuffers[kMaxVoices];

    // RT-safe state
    double sampleRate = 0.0;
    int samplesPerBlock = 512;
    int numChannels = 2;
};
