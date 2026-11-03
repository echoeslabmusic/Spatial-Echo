// BinauralProcessor: 3D spatial audio using HRTFs; core binaural rendering engine.
// Realtime-safety: no allocation/locks in processBlock()

#pragma once

#include <JuceHeader.h>
#include <vector>
#include <memory>
#include <atomic>
#include <array>
#include <complex>

struct MYSOFA_HRTF;
struct MYSOFA_EASY;

/**
 * BinauralProcessor - 3D spatial audio processor using HRTFs
 *
 * This processor applies Head-Related Transfer Functions to position
 * audio in 3D space using binaural processing techniques.
 */
class BinauralProcessor
{
public:
    BinauralProcessor();
    ~BinauralProcessor();

    // Core processing functions
    void prepare(double sampleRate, int samplesPerBlock, int numChannels);
    void reset();
    // Auto-panic flush: reset only the IIR filter state that can latch a NaN
    // indefinitely. FIR convolution / ITD / early-reflection delay histories
    // self-heal within their memory length once upstream is clean, so they are
    // left untouched (full reset() is reserved for the prepare path).
    void clearNaNLatchableState();
    void processBlock(juce::AudioBuffer<float>& buffer);

    // Parameter update functions
    void setEnabled(bool enabled);
    void setAzimuth(float azimuthDegrees);
    void setElevation(float elevationDegrees);
    void setDistance(float distanceMeters);
    void setWetMix(float wetPercent);
    void setCrossoverFrequency(float freqHz);
    float getCrossoverFrequency() const { return crossoverFrequency.load(); }

    // Snap to position immediately (no interpolation ramp).
    // Use after prepare() to initialize multi-voice spatial positions without ramp artifacts.
    void snapToPosition(float azimuthDegrees, float elevationDegrees, float distanceMeters = 1.0f);

    // HRTF Quality Mode. Performance uses the nearest measured HRIR, Balanced
    // and Quality interpolate between measurements; each mode also caps the
    // HRIR length (see HRIR_PERF_CAP below).
    enum class HRTFQuality
    {
        Performance,
        Balanced,
        Quality
    };
    void setHRTFQuality(HRTFQuality quality) { hrtfQuality.store(static_cast<int>(quality)); }
    HRTFQuality getHRTFQuality() const { return static_cast<HRTFQuality>(hrtfQuality.load()); }

    bool isHrtfLoaded() const;

    // =========================================================================
    // Advanced Binaural Features
    // =========================================================================

    // Diffuse-field EQ. On by default.
    // Removes the direction-independent colouration of the HRTF set: the power
    // average of the bundled dataset over all 440 directions is +13.3 dB at
    // 4 kHz and +9.8 dB at 3 kHz, which the binaural path would otherwise
    // impose on everything it renders. See BinauralProcessorDiffuse.cpp.
    void setDiffuseFieldEQEnabled(bool enabled) { diffuseFieldEQEnabled.store(enabled); }
    bool getDiffuseFieldEQEnabled() const { return diffuseFieldEQEnabled.load(); }

    // Source Width/Spread
    // 0% = focused point source, 100% = fully diffuse/ambient
    void setSourceWidth(float widthPercent);
    float getSourceWidth() const { return sourceWidth.load(); }

    enum class SofaDataDelayShape
    {
        Missing,
        Scalar,
        PerReceiver,
        PerSource,
        PerSourcePerReceiver,
        Unsupported
    };

    struct SofaConventionReport
    {
        bool usingSofa = false;
        int numSources = 0;
        int numReceivers = 0;
        int irLength = 0;
        int gridPoints = 0;
        int leftReceiverIndex = -1;
        int rightReceiverIndex = -1;
        bool receiverSwapApplied = false;
        bool azimuthSignFlipped = false;
        SofaDataDelayShape dataDelayShape = SofaDataDelayShape::Missing;
        int dataDelayElements = 0;
        int dataDelayNonZeroCount = 0;
        int dataDelayNonFiniteCount = 0;
        float dataDelayMinSeconds = 0.0f;
        float dataDelayMaxSeconds = 0.0f;
        SofaDataDelayShape normalizedDataDelayShape = SofaDataDelayShape::Missing;
        int normalizedDataDelayElements = 0;
        int normalizedDataDelayNonZeroCount = 0;
        int normalizedDataDelayNonFiniteCount = 0;
        float normalizedDataDelayMinSeconds = 0.0f;
        float normalizedDataDelayMaxSeconds = 0.0f;
        bool orientationProbeFound = false;
        float orientationProbeRawAzimuthDeg = 0.0f;
        float orientationProbeMappedAzimuthDeg = 0.0f;
        float orientationProbeElevationDeg = 0.0f;
    };

    // Getters for current state
    bool getEnabled() const { return isEnabled.load(); }
    float getAzimuth() const { return currentAzimuth.load(); }
    float getElevation() const { return currentElevation.load(); }
    float getDistance() const { return currentDistance.load(); }
    float getWetMix() const { return wetMix.load(); }

    // =========================================================================
    // Diagnostics read by the test suite
    // =========================================================================
    struct Diagnostics
    {
        // Band levels (RMS in dB)
        float lowBandDb = -100.0f;
        float highBandDb = -100.0f;

        // SLERP interpolation state
        float slerpT = 1.0f;  // 0=interpolating, 1=at target
        float slerpAzimuth = 0.0f;
        float slerpElevation = 0.0f;

        // Near-field safety trim
        float nearFieldSafetyGain = 1.0f;  // 1 = no trim, <1 = safety trim active
        float nearFieldPreSafetyPeak = 0.0f;
        float nearFieldPostSafetyPeak = 0.0f;

        // SOFA lookup state of the last HRIR update (delays in seconds)
        bool usingSofa = false;
        bool sofaInterpolated = false;
        float sofaDelaySecondsL = 0.0f;
        float sofaDelaySecondsR = 0.0f;
    };

    const Diagnostics& getDiagnostics() const { return diagnostics; }
    void setDiagnosticsEnabled(bool enabled) { diagnosticsEnabled = enabled; }

    // HRTF loading. The bundled dataset is embedded in the binary; a SOFA file
    // can replace it, and a failed load restores the bundled dataset.
    bool loadSOFAFile(const juce::File& sofaFile);
    void useDefaultHRTF();

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    bool isUsingDefaultHrtfForTest() const noexcept { return usingDefaultHrtf; }
    int getCurrentHrirLengthForTest() const noexcept;
    int getRawHrirLengthForTest() const noexcept;
    int getConvolutionPreparedIrCapacityForTest() const noexcept;
    int getConvolutionActiveIrLengthForTest() const noexcept;
    int getConvolutionFftSizeForTest() const noexcept;
    float getTargetHrirTailRmsForTest(int startSample) const noexcept;
    SofaConventionReport getSofaConventionReportForTest() const;
    bool loadSofaHrtfForTest(MYSOFA_HRTF* hrtf);
    void primeSpatialStateForTest();
    void processEarlyReflectionsForTest(juce::AudioBuffer<float>& buffer, float distance)
    {
        processEarlyReflections(buffer, distance);
    }

    // Test isolation switches: each bypasses one stage so a test can measure
    // the rest. They do not exist in plug-in builds.
    void setDebugDisableDiffuseEQ(bool disabled) { setTestBypass(TestBypass::DiffuseEqAlignment, disabled); }
    void setDebugDisableWidth(bool disabled) { setTestBypass(TestBypass::Width, disabled); }
    void setDebugDisableAirAbsorption(bool disabled) { setTestBypass(TestBypass::AirAbsorption, disabled); }
    void setDebugDisableEarlyReflections(bool disabled) { setTestBypass(TestBypass::EarlyReflections, disabled); }
    void setDebugDisableNearFieldDvfForTest(bool disabled) { setTestBypass(TestBypass::NearFieldDvf, disabled); }
    void setDebugDisableNearFieldSafetyForTest(bool disabled) { setTestBypass(TestBypass::NearFieldSafety, disabled); }
    // Disabling HRIR interpolation selects the nearest measured HRIR in every
    // quality mode, as Performance always does.
    void setHrirInterpolationEnabledForTest(bool enabled) { setTestBypass(TestBypass::HrirInterpolation, !enabled); }
#endif

private:
#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    // Self-tests in Tests/Binaural reach the convolution engine, the SLERP
    // helper and the convolvers directly.
    friend struct BinauralProcessorTestAccess;
#endif

    // DiffuseEqAlignment leaves the diffuse-field EQ itself on and drops its
    // group delay from the crossover alignment.
    enum class TestBypass
    {
        DiffuseEqAlignment,
        Width,
        AirAbsorption,
        EarlyReflections,
        NearFieldDvf,
        NearFieldSafety,
        HrirInterpolation,
        Count
    };
#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    std::array<std::atomic<bool>, static_cast<size_t>(TestBypass::Count)> testBypass{};
    void setTestBypass(TestBypass stage, bool bypassed) noexcept
    {
        testBypass[static_cast<size_t>(stage)].store(bypassed);
    }
    bool isBypassedForTest(TestBypass stage) const noexcept { return testBypass[static_cast<size_t>(stage)].load(); }
#else
    static constexpr bool isBypassedForTest(TestBypass) noexcept { return false; }
#endif

    Diagnostics diagnostics;
#if JUCE_DEBUG
    bool diagnosticsEnabled = true;  // Enable by default in debug builds
#else
    bool diagnosticsEnabled = false;
#endif

    // HRIR length cap per quality mode: Performance HRIR_PERF_CAP, Balanced
    // HRIR_BALANCED_CAP, Quality HRIR_MAX_LENGTH. The bundled HUTUBS set is 256
    // taps at 44.1 kHz and is resampled to the host rate on load, which scales
    // its length, so Quality keeps the whole measured response up to 88.2 kHz.
    // Convolution uses a 64-tap direct head and fixed 64-sample FFT tail hops,
    // and host callback boundaries add no buffering delay to either path.
    static constexpr int HRIR_DEFAULT_LENGTH = 200;  // Placeholder until a dataset loads
    static constexpr int HRIR_MAX_LENGTH = 512;
    static constexpr int HRIR_FFT_MIN_SIZE = 256;  // Minimum FFT size for the diffuse-field analysis
    static constexpr int HRIR_PERF_CAP = 128;
    static constexpr int HRIR_BALANCED_CAP = 256;
    struct HRIRData
    {
        std::array<float, HRIR_MAX_LENGTH> leftEar;
        std::array<float, HRIR_MAX_LENGTH> rightEar;
    };

    // Processing components
    // Each convolver crossfades internally when its IR changes.
    class ConvolutionEngine;
    std::unique_ptr<ConvolutionEngine> leftEarConvA;
    std::unique_ptr<ConvolutionEngine> rightEarConvA;
    std::unique_ptr<ConvolutionEngine> sideLeftEarConvA;
    std::unique_ptr<ConvolutionEngine> sideRightEarConvA;

    // Crossover for HRTF processing
    // Only frequencies ABOVE crossover get HRTF processing
    // Bass (<150Hz) passes through unchanged - provides stable anchor during transitions
    // Default is 150 Hz; can be adjusted via the BinauralCrossover parameter.
    static constexpr float kLowMidCrossover = 150.0f;

    // Crossover filter pairs (Linkwitz-Riley 4th order for flat summing)
    juce::dsp::LinkwitzRileyFilter<float> lowMidLowPass;   // Extracts low band
    juce::dsp::LinkwitzRileyFilter<float> lowMidHighPass;  // Passes mid+high

    // Multi-band air absorption (frequency-dependent, increases with distance)
    // Higher frequencies are absorbed more: ~0.002 dB/m at 1kHz, ~0.08 dB/m at 16kHz
    juce::dsp::StateVariableTPTFilter<float> airAbsorptionLowL;  // Gentle rolloff
    juce::dsp::StateVariableTPTFilter<float> airAbsorptionLowR;
    juce::dsp::StateVariableTPTFilter<float> airAbsorptionHighL;  // Stronger rolloff
    juce::dsp::StateVariableTPTFilter<float> airAbsorptionHighR;

    // Near-Field Distance Variation Function (DVF) - Spagnol et al. (2017)
    // A first-order shelf per ear simulates near-field effects below 1 m (bass
    // boost from the proximity effect, larger ILD at close range). Coefficients
    // come from precomputed lookup tables, so no allocation per block.
    float nearFieldGainSmoothAlpha = 0.0f;
    float nearFieldCompensationGain = 1.0f;  // Common trim for modeled near-field boost
    float nearFieldCompensationTarget = 1.0f;
    float nearFieldSafetyEnv = 0.0f;   // Peak envelope for near-field safety trim
    float nearFieldSafetyGain = 1.0f;  // Smoothed near-field safety gain
    float nearFieldSafetyAttackAlpha = 0.0f;
    float nearFieldSafetyReleaseAlpha = 0.0f;
    // Close-field stress guard: a peak limiter that only acts inside 1 m.
    //
    // The threshold assumes a correctly normalised convolution path (see
    // OouraFFT::ifft). A 0.60 threshold would start shaving ~4 dB off any
    // near-field signal that merely reaches unity, and would sit on its -9 dB
    // floor (i.e. out of control, not a safety net) on hot material.
    // 0.95 keeps the guard just below full scale, which is what a safety net
    // should do, and preserves its recovery margin.
    static constexpr float kNearFieldSafetyThreshold = 0.95f;
    static constexpr float kNearFieldSafetyMinGain = 0.35f;  // ~ -9 dB max trim

    // DVF filter coefficients (smoothed to avoid zipper/clicking)
    float dvfCoeffsL[3] = {1.0f, 0.0f, 0.0f};  // b0, b1, a1 (current)
    float dvfCoeffsR[3] = {1.0f, 0.0f, 0.0f};
    float dvfCoeffsTargetL[3] = {1.0f, 0.0f, 0.0f};  // target coefficients
    float dvfCoeffsTargetR[3] = {1.0f, 0.0f, 0.0f};
    float dvfCoeffSmoothAlpha = 0.0f;
    float dvfStateL[2] = {0.0f, 0.0f};  // Filter state
    float dvfStateR[2] = {0.0f, 0.0f};

    // RT-safe DVF coefficient calculation from pre-computed lookup tables
    void updateNearFieldDVF(float distanceMeters);
    void calcDVFCoeffsRT(float alpha, float rho, float* b0, float* b1, float* a1);
    float processDVFSample(float input, float* coeffs, float* state);

    // Early reflection tap cluster for room ambience
    static constexpr int NUM_ER_TAPS = 6;
    struct EarlyReflectionTap
    {
        float delayMs;  // Delay time in milliseconds
        float gainL;    // Left channel gain (linear)
        float gainR;    // Right channel gain (linear)
    };
    std::array<EarlyReflectionTap, NUM_ER_TAPS> erTaps;
    std::array<juce::AudioBuffer<float>, NUM_ER_TAPS> erDelayBuffers;
    std::array<int, NUM_ER_TAPS> erWritePositions;
    int maxErDelaySamples = 0;
    std::array<float, NUM_ER_TAPS> erDelaySamplesCurrent{};
    std::array<float, NUM_ER_TAPS> erGainLCurrent{};
    std::array<float, NUM_ER_TAPS> erGainRCurrent{};
    float erDelaySmoothAlpha = 1.0f;
    float erGainSmoothAlpha = 1.0f;
    bool erSmoothingInitialized = false;

    void initEarlyReflections();
    void processEarlyReflections(juce::AudioBuffer<float>& buffer, float distance);

    // HRTF database management
    class HRTFDatabase;
    std::unique_ptr<HRTFDatabase> hrtfDatabase;
    bool usingDefaultHrtf = false;
    std::unique_ptr<class OouraFFT> hrirFFT;

    // HRIR currently loaded into the convolvers
    HRIRData targetHRIR;

    // Smooth parameter interpolation
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> azimuthSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> elevationSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> distanceSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> wetMixSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> sourceWidthSmoother;

    // Atomic parameters for thread-safety
    std::atomic<bool> isEnabled{false};
    std::atomic<float> currentAzimuth{0.0f};
    std::atomic<float> currentElevation{0.0f};
    std::atomic<float> currentDistance{1.0f};
    std::atomic<float> wetMix{1.0f};
    std::atomic<int> hrtfQuality{2};  // 0=Performance, 1=Balanced, 2=Quality (default)
    std::atomic<bool> adaptivePerformanceModeActive{false};

    // Advanced binaural feature atomics
    std::atomic<bool> diffuseFieldEQEnabled{true};  // See BinauralProcessorDiffuse.cpp
    std::atomic<float> sourceWidth{0.0f};           // 0-100%, 0 = point source
    std::atomic<float> crossoverFrequency{150.0f};

    // Processing state
    double sampleRate = 44100.0;
    int blockSize = 512;
    int numChannels = 2;

    // Internal buffers for crossover processing
    juce::AudioBuffer<float> monoBuffer;
    juce::AudioBuffer<float> lowFreqBuffer;   // <150Hz - bass anchor
    juce::AudioBuffer<float> highFreqBuffer;  // >=150Hz - HRTF processing
    juce::AudioBuffer<float> tempBuffer;      // Scratch (wet-mix ramp)
    juce::AudioBuffer<float> wetBuffer;
    juce::AudioBuffer<float> convBuffer;      // HRTF convolution output (avoid per-block alloc)
    juce::AudioBuffer<float> sideBuffer;      // Stereo side signal for HRTF processing
    juce::AudioBuffer<float> sideConvBuffer;  // HRTF-processed side output
    juce::AudioBuffer<float> erInputBuffer;   // Pre-ER snapshot for independent reflection taps
    juce::AudioBuffer<float> erAccumBuffer;   // Summed ER output before adding to wet path

    // Low-band ITD delay (applied to bass anchor for realistic timing cues)
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> lowBandDelaySmootherL;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> lowBandDelaySmootherR;
    std::vector<float> lowBandDelayBuffer;
    int lowBandDelayWritePos = 0;
    int lowBandDelayBufferSize = 0;
    float currentItdSeconds = 0.0f;

    // CROSSOVER ALIGNMENT.
    //
    // The low band bypasses the HRTF convolution entirely, so unless it is
    // delayed to match, the two halves of the Linkwitz-Riley pair do not sum
    // flat and the stage digs a notch at the crossover frequency. MEASURED on
    // the bundled dataset with no compensation: -7.2 dB at 150 Hz.
    //
    // The delay to match is the HRTF path's GROUP DELAY at the crossover
    // frequency, which is not the same thing as the HRIR's onset. Measured
    // HRIRs carry substantial excess phase at low frequency: the bundled set
    // has a leading-ear onset of only ~22 samples @44.1 kHz but a group delay
    // of ~93 samples (2.12 ms) at 150 Hz. Using the onset under-compensates by
    // a factor of four.
    //
    // Because the crossover frequency is a live parameter (20-500 Hz), the
    // delay is measured at load across that range and interpolated at runtime;
    // it varies materially over the range (measured 94 / 101 / 88 / 59 samples
    // at 60 / 150 / 300 / 500 Hz @44.1 kHz). The DFE correction filter's own
    // group delay is tabulated separately so it can be added only when the DFE
    // is actually enabled. The convolver adds no algorithmic latency.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> lowBandAlignSmoother;
    static constexpr int kAlignTableSize = 8;
    static const std::array<float, kAlignTableSize> kAlignTableFreqHz;
    std::array<float, kAlignTableSize> hrirAlignDelayTable{};  // HRIR only, samples
    std::array<float, kAlignTableSize> dfeAlignDelayTable{};   // DFE FIR only, samples
    bool alignTableValid = false;
    void computeCrossoverAlignmentTable();
    float lookupCrossoverAlignmentSamples(float freqHz) const;
    void updateLowBandAlignment();
    // 2.2 ms of headroom: covers the measured ~2.1 ms HRIR group delay at any
    // supported sample rate up to 192 kHz, plus one convolution partition.
    static constexpr int kMaxLowBandAlignSamples = 512;

    // Helper functions
    void updateHRIRFromPosition(int numSamplesForSmoothing = 0);
    float calculateDistanceAttenuation(float distance);
    // Air-absorption lowpasses must stay below Nyquist to remain stable.
    float getMaxAirAbsorptionCutoff() const noexcept
    {
        return 0.45f * static_cast<float>(sampleRate > 0.0 ? sampleRate : 44100.0);
    }
    void updateLowBandITDTargets(float itdSeconds);
    void applyLowBandITD(const float* input, float* outL, float* outR, int numSamples);

    // =========================================================================
    // Advanced Binaural Feature Helpers
    // =========================================================================

    // Diffuse-Field EQ (power-average over directions, inverted, realised as a
    // short minimum-phase FIR that is convolved into each HRIR).
    void computeDiffuseFieldResponse();  // Called once when HRTF database loads
    void applyDiffuseFieldEQ(HRIRData& hrir);
    static constexpr int kDfeFirLength = 64;                   // 99.998% of the correction energy
    std::array<float, HRIR_MAX_LENGTH> diffuseFieldResponseL;  // Min-phase FIR, left ear
    std::array<float, HRIR_MAX_LENGTH> diffuseFieldResponseR;  // Min-phase FIR, right ear
    int diffuseFieldFirLength = 0;
    bool diffuseFieldComputed = false;

    // Source width.
    //
    // The spread target is the focused HRIR run through a per-ear
    // energy-normalised decorrelation sequence, NOT an average of HRIRs over a
    // cone of directions. Averaging HRIRs coherently cancels: measured on the
    // bundled dataset, the all-directions average sits 20 dB below the focused
    // HRIR at 4.8 kHz and 24 dB below at 9.6 kHz, so such a "spread" component
    // is a lowpass with a completely different spectrum from the thing it is
    // blended with. No scalar normalisation can fix that - matching total
    // energy, or energy above the crossover, both leave the level several dB out
    // as soon as the input is not white, because the two components weight the
    // input spectrum differently. Decorrelating the focused HRIR instead keeps
    // the magnitude response (and therefore the level) while removing the
    // interaural coherence, which is what "wide/diffuse" actually means.
    static constexpr int kSpreadDecorrLength = 64;
    std::array<float, kSpreadDecorrLength> spreadDecorrL{};
    std::array<float, kSpreadDecorrLength> spreadDecorrR{};
    void buildSpreadDecorrelators();
    void applySourceWidth(HRIRData& focused, float widthFactor);

    // ITD from the 750 Hz-lowpassed interaural cross-correlation, in samples
    // (positive = right ear leads).
    float estimateITDSamples(const HRIRData& hrir) const;
    void applyFractionalDelay(float* ir, int length, float delaySamples);

    // Pre-computed ITDs for grid points (computed once at load with cross-correlation)
    std::vector<float> precomputedITDs;  // One ITD per grid point in seconds
    bool itdsPrecomputed = false;
    void precomputeGridITDs();  // Called once after HRTF database loads
    float interpolateITD(float azimuth, float elevation) const;

    // ITD bounds (±0.707ms max, physically plausible for human head)
    static constexpr float MAX_ITD_SECONDS = 0.000707f;  // ~31 samples at 44.1kHz

    // Runtime HRIR sizing (set on dataset load)
    int hrirLengthRaw = HRIR_DEFAULT_LENGTH;
    int hrirLength = HRIR_DEFAULT_LENGTH;  // Effective (quality-capped) length
    int hrirFftSize = 0;
    int hrirFftBins = 0;
    void updateHrirFftConfig(int newLength);
    void onHrtfDatabaseChanged();
    int getEffectiveQualityMode(int requestedQualityMode) noexcept;
    void applyQualityHrirCap(int qualityMode);
    bool evaluateAdaptivePerformanceMode() noexcept;
    static constexpr double kAdaptiveEnterSampleRate = 96000.0;
    static constexpr double kAdaptiveExitSampleRate = 88200.0;
    static constexpr int kAdaptiveEnterBlockSize = 64;
    static constexpr int kAdaptiveExitBlockSize = 96;
    static int nextPow2(int value);

    // IR updates are gated by position-change thresholds and cooldowns.
    float lastIRUpdateAz = 0.0f;
    float lastIRUpdateEl = 0.0f;
    float lastIRUpdateWidth = 0.0f;
    int lastIRUpdateQualityMode = -1;
    int lastIRUpdateHrirLength = 0;
    int lastIRUpdateInterpolation = -1;  // -1 forces the first update; then 0 or 1

    // Distance-aware azimuth smoothing to reduce center-jitter artifacts
    float azimuthFiltered = 0.0f;
    bool azimuthFilterInitialized = false;

    // HRIR update rate limiting to avoid rapid time-varying convolution artifacts
    int hrirUpdateCooldownSamples = 0;

    // Distance smoothing for close-range modulation artifacts
    float distanceFiltered = 1.0f;
    bool distanceFilterInitialized = false;

    // Suppress early reflections briefly during rapid HRTF updates
    int erSuppressionSamples = 0;
    int erSuppressionSamplesMax = 0;
    void applyFrontMedianPlaneIldGuard(HRIRData& hrir, float azimuth, float elevation, bool interpolate);

    // SLERP interpolation for smooth azimuth/elevation transitions
    struct SphericalCoord
    {
        float azimuth;    // degrees, -180 to 180
        float elevation;  // degrees, -90 to 90
    };
    static SphericalCoord slerpPosition(const SphericalCoord& from, const SphericalCoord& to, float t);
    static float angularDistanceDegrees(float a, float b);
    SphericalCoord getCurrentSlerpPosition() const;
    void advancePositionInterpolation(int numSamples);
    float calculateDistanceAwareAzimuthAlpha(float distanceMeters, int numSamples) const;

    // Previous position for SLERP interpolation
    SphericalCoord previousPosition{0.0f, 0.0f};
    SphericalCoord targetPosition{0.0f, 0.0f};
    SphericalCoord effectiveRenderPosition{0.0f, 0.0f};
    float positionInterpolation = 1.0f;  // 0 = at previous, 1 = at target
    static constexpr double kPositionInterpolationTimeSeconds = 0.002;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BinauralProcessor)
};

#include "BinauralConvolutionEngine.h"
#include "BinauralHRTFDatabase.h"
