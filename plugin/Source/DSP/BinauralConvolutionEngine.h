// ConvolutionEngine: Zero-latency FIR head with partitioned overlap-save tail.
// Realtime-safety: no allocation/locks in process() or setImpulseResponse()

#pragma once

/**
 * The first 64 HRIR taps run directly. Remaining taps use 64-sample FFT
 * partitions; their inherent one-hop delay gives us time to compute each tail
 * block before its first output sample is needed. Callback boundaries never
 * change the signal's timing, including one-sample and in-place processing.
 *
 * IR changes crossfade both head and tail over 64 output samples. A change
 * inside a hop refreshes its remaining tail from the retained input spectra;
 * a change during a fade starts from the currently blended filter.
 *
 * Internal module header: included at the end of BinauralProcessor.h.
 */
class BinauralProcessor::ConvolutionEngine
{
public:
    ConvolutionEngine();
    ~ConvolutionEngine();

    void prepare(double sampleRate, int maxBlockSize, int maxIrLength);
    void reset();
    void setImpulseResponse(const float* ir, int irLength, bool irChanged);
    void process(const float* input, float* output, int numSamples);

    // Algorithmic latency only; the HRIR retains its own physical timing cues.
    int getLatencySamples() const noexcept { return 0; }
    static constexpr int kMaxPartitionSize = 64;

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    int getPreparedIrCapacityForTest() const noexcept { return preparedIrCapacity; }
    int getActiveIrLengthForTest() const noexcept { return activeIrLength; }
    int getFftSizeForTest() const noexcept { return fftSize; }
#endif

private:
    int partitionSize = 0;
    int fftSize = 0;
    int numPartitions = 0;  // Tail partitions only; excludes the direct FIR head.
    int preparedIrCapacity = 0;
    int activeIrLength = 0;
    std::unique_ptr<class OouraFFT> oouraFFT;

    struct PartitionSet
    {
        std::array<float, kMaxPartitionSize> head{};
        std::array<float, kMaxPartitionSize> tail{};
        std::vector<std::vector<std::complex<float>>> partitionSpectrum;
        std::vector<float> zeroPaddedIR;
    };
    PartitionSet partitionSets[2];
    std::atomic<int> activeFilterIndex{0};
    std::atomic<int> previousFilterIndex{0};
    std::atomic<bool> transitionActive{false};
    int transitionSample = 0;
    bool hasValidFilter = false;

    // Duplicated ring provides a contiguous newest-to-oldest FIR history.
    std::array<float, kMaxPartitionSize * 2> headHistory{};
    int headWritePosition = 0;
    std::array<float, kMaxPartitionSize> inputHop{};
    int hopPosition = 0;
    std::vector<float> inputWindow;
    std::vector<std::vector<std::complex<float>>> inputDelayLine;
    int delayLineWriteIndex = 0;

    std::vector<std::complex<float>> inputDFT;
    std::vector<std::complex<float>> outputDFT;
    std::vector<float> outputBuffer;
    static constexpr float kDenormalFloor = 1e-30f;

    void processHop();
    void refreshTail(PartitionSet& filter);
    void flattenTransition();
    static float processHead(const float* history, const float* coefficients) noexcept;
    void multiplyAndAccumulate(const std::vector<std::complex<float>>& inputSpectrum,
                               const std::vector<std::complex<float>>& partitionSpectrum,
                               std::vector<std::complex<float>>& outputSpectrum);
    void applyDenormalGuard(std::vector<std::complex<float>>& spectrum);
};
