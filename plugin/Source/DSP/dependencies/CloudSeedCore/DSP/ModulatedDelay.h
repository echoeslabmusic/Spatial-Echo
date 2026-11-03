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

// [vendored] self-include removed
#include "Utils.h"
#include <algorithm>
#include <stdint.h>
#include <vector>

namespace Cloudseed
{
	class ModulatedDelay
	{
	private:

		static const int ModulationUpdateRate = 8;
		// [Spatial Echo] Placeholder capacity used only between construction and
		// the owner's SetCapacity() call, which happens inside the same
		// constructor chain. No audio is processed before that.
		static const int MinDelayBufferSize = 1024;

		// [Spatial Echo] Was `double delayBuffer[192000 * 2]`, i.e. a fixed
		// 3.07 MB member sized for 1 s at 192 kHz, doubled. That is both too
		// small above a 96 kHz host (the engine runs at 2x, and the late lines
		// ask for up to 1.5 s) and roughly 2.6x too large at 48 kHz. Sized from
		// the engine sample rate at prepare time instead; see
		// Cloudseed::Capacity in Utils.h.
		std::vector<double> delayBuffer;
		int DelayBufferSize;
		int writeIndex;
		int readIndexA;
		int readIndexB;
		uint64_t samplesProcessed;

		double modPhase;
		double gainA;
		double gainB;
		double realisedDelay;

	public:
		int SampleDelay;

		double ModAmount;
		double ModRate;

		ModulatedDelay()
			: delayBuffer(static_cast<size_t>(MinDelayBufferSize), 0.0)
			, DelayBufferSize(MinDelayBufferSize)
		{
			writeIndex = 0;
			readIndexA = 0;
			readIndexB = 0;
			samplesProcessed = 0;

			// [Spatial Echo] Was seeded from the process-global std::rand(), which
			// made the reverb's sound depend on unrelated global PRNG state.
			// The owner now assigns an explicit deterministic phase through
			// SetModulationPhase(); this is only the pre-assignment default.
			modPhase = 0.5;
			realisedDelay = 0.0;
			gainA = 1.0;
			gainB = 0.0;

			SampleDelay = 100;
			ModAmount = 0.0;
			ModRate = 0.0;

			Update();
		}

		void Process(double* input, double* output, int bufSize)
		{
			for (int i = 0; i < bufSize; i++)
			{
				if (samplesProcessed == ModulationUpdateRate)
					Update();

				delayBuffer[writeIndex] = input[i];
				output[i] = delayBuffer[readIndexA] * gainA + delayBuffer[readIndexB] * gainB;

				writeIndex++;
				readIndexA++;
				readIndexB++;
				if (writeIndex >= DelayBufferSize) writeIndex -= DelayBufferSize;
				if (readIndexA >= DelayBufferSize) readIndexA -= DelayBufferSize;
				if (readIndexB >= DelayBufferSize) readIndexB -= DelayBufferSize;
				samplesProcessed++;
			}
		}

		void ClearBuffers()
		{
			Utils::ZeroBuffer(delayBuffer.data(), DelayBufferSize);
		}

		// A feedback loop may read this many samples before writing their
		// inputs. Both interpolation taps then refer only to earlier samples.
		// Include the cached delay: parameter changes take effect at the next
		// modulation update, so its old read position can still be shorter.
		int GetFeedbackReadLimit() const
		{
			const double minimumDelay = std::min({ realisedDelay,
				static_cast<double>(SampleDelay) - std::abs(ModAmount),
				static_cast<double>(DelayBufferSize - 2) });
			return std::max(1, static_cast<int>(std::floor(minimumDelay)));
		}

		// Paired feedback access. The caller must bound bufSize by
		// GetFeedbackReadLimit() and call WriteFeedback immediately afterwards.
		// Advancing the same indices/clock as Process keeps modulation and
		// interpolation independent of callback or internal chunk boundaries.
		void ReadFeedback(double* output, int bufSize)
		{
			for (int i = 0; i < bufSize; i++)
			{
				if (samplesProcessed == ModulationUpdateRate)
					Update();
				output[i] = delayBuffer[readIndexA] * gainA + delayBuffer[readIndexB] * gainB;
				writeIndex++;
				readIndexA++;
				readIndexB++;
				if (writeIndex >= DelayBufferSize) writeIndex -= DelayBufferSize;
				if (readIndexA >= DelayBufferSize) readIndexA -= DelayBufferSize;
				if (readIndexB >= DelayBufferSize) readIndexB -= DelayBufferSize;
				samplesProcessed++;
			}
		}

		void WriteFeedback(const double* input, int bufSize)
		{
			int index = writeIndex - bufSize;
			if (index < 0) index += DelayBufferSize;
			const int firstSpan = std::min(bufSize, DelayBufferSize - index);
			std::memcpy(delayBuffer.data() + index, input,
				static_cast<size_t>(firstSpan) * sizeof(double));
			if (firstSpan < bufSize)
				std::memcpy(delayBuffer.data(), input + firstSpan,
					static_cast<size_t>(bufSize - firstSpan) * sizeof(double));
		}

		// [Spatial Echo] Prepare-time only. Never call this from the audio
		// thread: it allocates.
		void SetCapacity(int samples)
		{
			if (samples < MinDelayBufferSize)
				samples = MinDelayBufferSize;
			if (samples == DelayBufferSize)
				return;

			// assign() retains capacity after a sample-rate downshift. Replace
			// storage at prepare time so a prior high-rate session cannot pin
			// its worst-case allocation for the lifetime of this instance.
			std::vector<double>(static_cast<size_t>(samples), 0.0).swap(delayBuffer);
			DelayBufferSize = samples;
			writeIndex = 0;
			readIndexA = 0;
			readIndexB = 0;
			samplesProcessed = 0;
			Update();
		}

		// [Spatial Echo] Deterministic modulation phase, assigned by the owner.
		// value is taken modulo 1 and mapped into the same 0.01 .. 0.99 range
		// the original std::rand() seeding produced.
		void SetModulationPhase(double value01)
		{
			if (!(value01 >= 0.0) || !(value01 < 1.0))
				value01 = 0.0;
			modPhase = 0.01 + 0.98 * value01;
		}

		int GetCapacity() const { return DelayBufferSize; }
		int GetRequestedDelaySamples() const { return SampleDelay; }
		double GetRealisedDelaySamples() const { return realisedDelay; }
		size_t GetAllocatedBytes() const
		{
			return delayBuffer.capacity() * sizeof(double);
		}


	private:
		void Update()
		{
			modPhase += ModRate * ModulationUpdateRate;
			if (modPhase > 1.0)
				modPhase = std::fmod(modPhase, 1.0);

			auto mod = std::sin(modPhase * 2.0 * M_PI);
			auto totalDelay = SampleDelay + ModAmount * mod;

			// [Spatial Echo] Backstop clamp. The buffer is now sized from the
			// engine sample rate for the longest delay the vendored parameter
			// laws can request, so this cannot engage in normal operation; it
			// remains so an out-of-range caller still cannot read out of bounds.
			if (totalDelay > (double)(DelayBufferSize - 2))
				totalDelay = (double)(DelayBufferSize - 2);
			if (totalDelay < 1.0)
				totalDelay = 1.0;

			auto delayA = (int)totalDelay;
			auto delayB = (int)totalDelay + 1;

			auto partial = totalDelay - delayA;

			gainA = 1.0 - partial;
			gainB = partial;

			readIndexA = writeIndex - delayA;
			readIndexB = writeIndex - delayB;
			if (readIndexA < 0) readIndexA += DelayBufferSize;
			if (readIndexB < 0) readIndexB += DelayBufferSize;

			realisedDelay = totalDelay;
			samplesProcessed = 0;
		}
	};
}
