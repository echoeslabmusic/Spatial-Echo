// ConvolutionEngine: Zero-latency FIR head with partitioned overlap-save tail.
// Realtime-safety: no allocation/locks in process() or setImpulseResponse()

#include "BinauralProcessor.h"
#include "OouraFFT.h"
#include <algorithm>
#include <cmath>

BinauralProcessor::ConvolutionEngine::ConvolutionEngine()
    : oouraFFT(std::make_unique<OouraFFT>())
{
}

BinauralProcessor::ConvolutionEngine::~ConvolutionEngine() = default;

void BinauralProcessor::ConvolutionEngine::prepare(double /*sampleRate*/, int /*maxBlockSize*/, int maxIrLength)
{
    preparedIrCapacity = juce::jmax(1, maxIrLength);
    // A fixed hop bounds both direct-FIR and FFT work independently of host
    // preparation/callback size. The head supplies immediate output even when
    // the host prepares for one sample at a time.
    partitionSize = kMaxPartitionSize;
    fftSize = 2 * partitionSize;
    numPartitions = (preparedIrCapacity - 1) / partitionSize;
    const int binCount = fftSize / 2 + 1;
    oouraFFT->init(static_cast<size_t>(fftSize));

    for (auto& filter : partitionSets)
    {
        filter.head.fill(0.0f);
        filter.partitionSpectrum.resize(static_cast<size_t>(numPartitions));
        for (auto& spectrum : filter.partitionSpectrum)
            spectrum.assign(static_cast<size_t>(binCount), {});
        filter.zeroPaddedIR.assign(static_cast<size_t>(fftSize), 0.0f);
    }
    inputWindow.assign(static_cast<size_t>(fftSize), 0.0f);
    inputDelayLine.resize(static_cast<size_t>(numPartitions));
    for (auto& spectrum : inputDelayLine)
        spectrum.assign(static_cast<size_t>(binCount), {});
    inputDFT.resize(static_cast<size_t>(binCount));
    outputDFT.resize(static_cast<size_t>(binCount));
    outputBuffer.assign(static_cast<size_t>(fftSize), 0.0f);
    activeFilterIndex.store(0);
    previousFilterIndex.store(0);
    hasValidFilter = false;
    activeIrLength = 0;
    reset();
}

void BinauralProcessor::ConvolutionEngine::reset()
{
    for (auto& spectrum : inputDelayLine)
        std::fill(spectrum.begin(), spectrum.end(), std::complex<float>{});
    for (auto& filter : partitionSets)
        filter.tail.fill(0.0f);
    headHistory.fill(0.0f);
    inputHop.fill(0.0f);
    headWritePosition = 0;
    hopPosition = 0;
    delayLineWriteIndex = 0;
    std::fill(inputWindow.begin(), inputWindow.end(), 0.0f);
    std::fill(outputBuffer.begin(), outputBuffer.end(), 0.0f);
    transitionSample = 0;
    previousFilterIndex.store(activeFilterIndex.load());
    transitionActive.store(false);
}

void BinauralProcessor::ConvolutionEngine::flattenTransition()
{
    if (!transitionActive.load())
        return;

    // Preserve the filter that would be heard on the next sample before the
    // older set is reused. Linearity makes the same blend valid for cached
    // tail output, direct taps, and every frequency-domain partition.
    auto& active = partitionSets[activeFilterIndex.load()];
    const auto& previous = partitionSets[previousFilterIndex.load()];
    const float t = static_cast<float>(transitionSample) / static_cast<float>(partitionSize - 1);
    for (int i = 0; i < partitionSize; ++i)
    {
        active.head[static_cast<size_t>(i)] =
            previous.head[static_cast<size_t>(i)] +
            t * (active.head[static_cast<size_t>(i)] - previous.head[static_cast<size_t>(i)]);
        active.tail[static_cast<size_t>(i)] =
            previous.tail[static_cast<size_t>(i)] +
            t * (active.tail[static_cast<size_t>(i)] - previous.tail[static_cast<size_t>(i)]);
    }
    for (int p = 0; p < numPartitions; ++p)
    {
        auto& dst = active.partitionSpectrum[static_cast<size_t>(p)];
        const auto& src = previous.partitionSpectrum[static_cast<size_t>(p)];
        for (size_t bin = 0; bin < dst.size(); ++bin)
            dst[bin] = src[bin] + t * (dst[bin] - src[bin]);
    }
    transitionActive.store(false);
}

void BinauralProcessor::ConvolutionEngine::setImpulseResponse(const float* ir, int irLength, bool irChanged)
{
    if (fftSize == 0 || ir == nullptr || irLength <= 0 || !irChanged)
        return;

    flattenTransition();
    const int oldIndex = activeFilterIndex.load();
    const int targetIndex = hasValidFilter ? (oldIndex ^ 1) : oldIndex;
    auto& target = partitionSets[targetIndex];
    const int copyLength = juce::jmin(irLength, preparedIrCapacity);
    target.head.fill(0.0f);
    std::copy_n(ir, juce::jmin(copyLength, partitionSize), target.head.begin());

    for (int p = 0; p < numPartitions; ++p)
    {
        auto& padded = target.zeroPaddedIR;
        std::fill(padded.begin(), padded.end(), 0.0f);
        const int offset = (p + 1) * partitionSize;
        const int count = juce::jlimit(0, partitionSize, copyLength - offset);
        if (count > 0)
            std::copy_n(ir + offset, count, padded.begin());
        oouraFFT->fft(padded.data(), target.partitionSpectrum[static_cast<size_t>(p)].data());
    }
    // We may be in the middle of an output hop. Its input spectra are already
    // available because all tail coefficients have at least one hop of delay.
    refreshTail(target);
    previousFilterIndex.store(oldIndex);
    activeFilterIndex.store(targetIndex);
    activeIrLength = copyLength;
    transitionSample = 0;
    transitionActive.store(hasValidFilter);
    hasValidFilter = true;
}

void BinauralProcessor::ConvolutionEngine::multiplyAndAccumulate(
    const std::vector<std::complex<float>>& inputSpectrum, const std::vector<std::complex<float>>& partitionSpectrum,
    std::vector<std::complex<float>>& outputSpectrum)
{
    for (size_t i = 0; i < outputSpectrum.size(); ++i)
        outputSpectrum[i] += inputSpectrum[i] * partitionSpectrum[i];
}

void BinauralProcessor::ConvolutionEngine::applyDenormalGuard(std::vector<std::complex<float>>& spectrum)
{
    for (auto& bin : spectrum)
    {
        if (bin.real() >= -kDenormalFloor && bin.real() <= kDenormalFloor)
            bin.real(0.0f);
        if (bin.imag() >= -kDenormalFloor && bin.imag() <= kDenormalFloor)
            bin.imag(0.0f);
    }
}

void BinauralProcessor::ConvolutionEngine::refreshTail(PartitionSet& filter)
{
    if (numPartitions == 0)
    {
        filter.tail.fill(0.0f);
        return;
    }
    std::fill(outputDFT.begin(), outputDFT.end(), std::complex<float>{});
    for (int p = 0; p < numPartitions; ++p)
    {
        // writeIndex points to the NEXT input hop. Tail partition p starts at
        // IR tap (p+1)*hop, so it uses the window completed (p+1) hops ago.
        const int index = (delayLineWriteIndex + numPartitions - 1 - p) % numPartitions;
        multiplyAndAccumulate(inputDelayLine[static_cast<size_t>(index)],
                              filter.partitionSpectrum[static_cast<size_t>(p)], outputDFT);
    }
    applyDenormalGuard(outputDFT);
    oouraFFT->ifft(outputDFT.data(), outputBuffer.data());
    // Only the final hop is linearly valid in overlap-save. Ooura's wrapper
    // already normalizes the inverse; no extra gain belongs here.
    std::copy_n(outputBuffer.data() + fftSize - partitionSize, partitionSize, filter.tail.begin());
}

void BinauralProcessor::ConvolutionEngine::processHop()
{
    if (numPartitions == 0)
        return;
    std::copy(inputWindow.begin() + partitionSize, inputWindow.end(), inputWindow.begin());
    std::copy(inputHop.begin(), inputHop.end(), inputWindow.end() - partitionSize);
    oouraFFT->fft(inputWindow.data(), inputDFT.data());
    std::copy(inputDFT.begin(), inputDFT.end(), inputDelayLine[static_cast<size_t>(delayLineWriteIndex)].begin());
    delayLineWriteIndex = (delayLineWriteIndex + 1) % numPartitions;
    refreshTail(partitionSets[activeFilterIndex.load()]);
    if (transitionActive.load())
        refreshTail(partitionSets[previousFilterIndex.load()]);
}

float BinauralProcessor::ConvolutionEngine::processHead(const float* history, const float* coefficients) noexcept
{
    // Four independent accumulators allow vectorization without changing the
    // compiler's floating-point contract or allocating an aligned scratch copy.
    float a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f;
    for (int tap = 0; tap < kMaxPartitionSize; tap += 4)
    {
        a += history[tap] * coefficients[tap];
        b += history[tap + 1] * coefficients[tap + 1];
        c += history[tap + 2] * coefficients[tap + 2];
        d += history[tap + 3] * coefficients[tap + 3];
    }
    return (a + b) + (c + d);
}

void BinauralProcessor::ConvolutionEngine::process(const float* input, float* output, int numSamples)
{
    if (output == nullptr || numSamples <= 0)
        return;
    if (fftSize == 0 || input == nullptr || !hasValidFilter)
    {
        std::fill_n(output, numSamples, 0.0f);
        return;
    }
    const auto& active = partitionSets[activeFilterIndex.load()];
    const auto& previous = partitionSets[previousFilterIndex.load()];
    for (int i = 0; i < numSamples; ++i)
    {
        const float sample = input[i];  // Read before writing for in-place callers.
        headWritePosition = (headWritePosition + partitionSize - 1) % partitionSize;
        headHistory[static_cast<size_t>(headWritePosition)] = sample;
        headHistory[static_cast<size_t>(headWritePosition + partitionSize)] = sample;
        inputHop[static_cast<size_t>(hopPosition)] = sample;
        const float* history = headHistory.data() + headWritePosition;
        const float current = processHead(history, active.head.data()) + active.tail[static_cast<size_t>(hopPosition)];
        if (transitionActive.load())
        {
            const float old =
                processHead(history, previous.head.data()) + previous.tail[static_cast<size_t>(hopPosition)];
            const float t = static_cast<float>(transitionSample) / static_cast<float>(partitionSize - 1);
            output[i] = old + t * (current - old);
            if (++transitionSample == partitionSize)
                transitionActive.store(false);
        }
        else
            output[i] = current;

        if (++hopPosition == partitionSize)
        {
            hopPosition = 0;
            processHop();
        }
    }
}
