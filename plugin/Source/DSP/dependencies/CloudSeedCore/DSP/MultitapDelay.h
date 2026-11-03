/*
Copyright (c) 2024 Ghost Note Engineering Ltd

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#pragma once

#include <memory>
#include <array>
#include <cmath>
#include <vector>
#include "Utils.h"
#include "RandomBuffer.h"

namespace Cloudseed
{
	class MultitapDelay
	{
	public:
		static const int MaxTaps = 256;
		// [Spatial Echo] Placeholder capacity used only between construction and
		// the owner's SetCapacity() call, which happens inside the same
		// constructor chain. No audio is processed before that.
		static const int MinDelayBufferSize = 1024;

	private:
		// [Spatial Echo] Was `double delayBuffer[192000 * 2]`, a fixed 3.07 MB
		// member. The longest tap length the vendored parameter law can ask for
		// is 1000 ms, so at the engine's 2x rate this only needs 1 s of the
		// engine sample rate. Sized at prepare time; see Cloudseed::Capacity in
		// Utils.h. (Unlike the late lines and the diffuser allpasses this one
		// was never truncated in practice: Spatial Echo pins TapLength to 0.3,
		// i.e. 307 ms. It was pure memory waste.)
		std::vector<double> delayBuffer;
		int DelayBufferSize;

		double tapGains[MaxTaps] = { 0 };
		double tapPosition[MaxTaps] = { 0 };

		std::array<float, MaxTaps * 3> seedValues{};

		int writeIdx;
		int seed;
		double crossSeed;
		int count;
		double lengthSamples;
		double decay;

	public:
		MultitapDelay()
			: delayBuffer(static_cast<size_t>(MinDelayBufferSize), 0.0)
			, DelayBufferSize(MinDelayBufferSize)
		{
			writeIdx = 0;
			seed = 0;
			crossSeed = 0.0;
			count = 1;
			lengthSamples = 1000.0;
			decay = 1.0;
			
			UpdateSeeds();
		}

		void SetSeed(int seed)
		{
			this->seed = seed;
			UpdateSeeds();
		}

		void SetCrossSeed(double crossSeed)
		{
			this->crossSeed = crossSeed;
			UpdateSeeds();
		}

		void SetTapCount(int tapCount)
		{
			if (tapCount < 1) tapCount = 1;
			count = tapCount;
			Update();
		}

		void SetTapLength(int tapLengthSamples)
		{
			if (tapLengthSamples < 10) tapLengthSamples = 10;
			// [Spatial Echo] Backstop clamp. The buffer is now sized for the
			// longest tap length the vendored parameter law can request, so this
			// cannot engage in normal operation; it remains so an out-of-range
			// caller still cannot read out of bounds.
			if (tapLengthSamples > DelayBufferSize - 1)
				tapLengthSamples = DelayBufferSize - 1;
			lengthSamples = tapLengthSamples;
			Update();
		}

		void SetTapDecay(double tapDecay)
		{
			decay = tapDecay;
		}

		void Process(double* input, double* output, int bufSize)
		{
			double lengthScaler = lengthSamples / (double)count;
			double totalGain = 3.0 / std::sqrt(1.0 + count);
			totalGain *= (1.0 + decay * 2.0);

			for (int i = 0; i < bufSize; i++)
			{
				delayBuffer[writeIdx] = input[i];
				output[i] = 0.0;

				for (int j = 0; j < count; j++)
				{
					double offset = tapPosition[j] * lengthScaler;
					double decayEffective = std::exp(-offset / lengthSamples * 3.3) * decay + (1.0 - decay);
					int readIdx = writeIdx - (int)offset;
					if (readIdx < 0) readIdx += DelayBufferSize;

					output[i] += delayBuffer[readIdx] * tapGains[j] * decayEffective * totalGain;
				}

				writeIdx = (writeIdx + 1) % DelayBufferSize;
			}
		}

		void ClearBuffers()
		{
			Utils::ZeroBuffer(delayBuffer.data(), DelayBufferSize);
		}

		// [Spatial Echo] Prepare-time only. Never call this from the audio
		// thread: it allocates.
		void SetCapacity(int samples)
		{
			if (samples < MinDelayBufferSize)
				samples = MinDelayBufferSize;
			if (samples == DelayBufferSize)
				return;

			// Release high-rate capacity when preparing at a lower rate.
			std::vector<double>(static_cast<size_t>(samples), 0.0).swap(delayBuffer);
			DelayBufferSize = samples;
			writeIdx = 0;
			// Re-apply the length so the backstop clamp is evaluated against
			// the new capacity.
			SetTapLength((int)lengthSamples);
		}

		int GetCapacity() const { return DelayBufferSize; }
		double GetRealisedTapLengthSamples() const { return lengthSamples; }
		size_t GetAllocatedBytes() const
		{
			return delayBuffer.capacity() * sizeof(double);
		}


	private:
		void Update()
		{
			int s = 0;
			auto rand = [&]() {return seedValues[s++]; };

			for (int i = 0; i < MaxTaps; i++)
			{
				double phase = rand() < 0.5 ? 1.0 : -1.0;
				tapGains[i] = Utils::DB2Gain(-20.0 + rand() * 20.0) * phase;
				tapPosition[i] = i + rand();
			}
		}

		void UpdateSeeds()
		{
			RandomBuffer::Fill(static_cast<uint64_t>(seed),
			                   static_cast<int>(seedValues.size()),
			                   static_cast<float>(crossSeed),
			                   seedValues.data());
			Update();
		}
	};
}
