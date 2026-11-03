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
#include <cmath>
#include <vector>

namespace Cloudseed
{
	class ModulatedAllpass
	{
	public:
		static const int ModulationUpdateRate = 8;
		// [Spatial Echo] Placeholder capacity used only between construction and
		// the owner's SetCapacity() call, which happens inside the same
		// constructor chain. No audio is processed before that.
		static const int MinDelayBufferSize = 256;

	private:
		// [Spatial Echo] Was `double delayBuffer[19200]`, commented "100ms at
		// 192Khz". The engine actually runs at 2x the host rate, so a 96 kHz
		// host already reaches that capacity exactly and 176.4 / 192 kHz hosts
		// overrun it: measured, the modulated path clamped a requested 30830
		// samples to 19198, and the un-modulated path WRAPPED it to 11630. At
		// the vendored parameter maximum (100 ms at a 384 kHz engine rate =
		// 38400 samples) the un-modulated path read out of bounds, confirmed by
		// AddressSanitizer. Sized from the engine sample rate at prepare time
		// instead; see Cloudseed::Capacity in Utils.h.
		std::vector<double> delayBuffer;
		int DelayBufferSize;
		int index;
		uint64_t samplesProcessed;

		double modPhase;
		int delayA;
		int delayB;
		double gainA;
		double gainB;
		double realisedDelay;

	public:

		int SampleDelay;
		double Feedback;
		double ModAmount;
		double ModRate;
		double Drive;

		bool InterpolationEnabled;
		bool ModulationEnabled;

		ModulatedAllpass()
			: delayBuffer(static_cast<size_t>(MinDelayBufferSize), 0.0)
			, DelayBufferSize(MinDelayBufferSize)
		{
			index = DelayBufferSize - 1;
			samplesProcessed = 0;

			// [Spatial Echo] Was seeded from the process-global std::rand(),
			// which made the reverb's sound depend on unrelated global PRNG
			// state. The owner now assigns an explicit deterministic phase
			// through SetModulationPhase(); this is only the pre-assignment
			// default.
			modPhase = 0.5;
			realisedDelay = 0.0;
			delayA = 0;
			delayB = 0;
			gainA = 1.0;
			gainB = 0.0;

			SampleDelay = 100;
			Feedback = 0.5;
			ModAmount = 0.0;
			ModRate = 0.0;
			Drive = 0.0;

			InterpolationEnabled = true;
			ModulationEnabled = true;
			Update();
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
			index = DelayBufferSize - 1;
			samplesProcessed = 0;
			Update();
		}

		// [Spatial Echo] Deterministic modulation phase, assigned by the owner,
		// mapped into the same 0.01 .. 0.99 range the original std::rand()
		// seeding produced.
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

		void Process(double* input, double* output, int sampleCount)
		{
			if (ModulationEnabled)
				ProcessWithMod(input, output, sampleCount);
			else
				ProcessNoMod(input, output, sampleCount);
		}

	private:
		static inline double SoftClip(double x, double drive)
		{
			x = x * drive;
			if (x > 1.0) x = 1.0;
			else if (x < -1.0) x = -1.0;
			return (x - x * x * x / 3.0) / drive;
		}

		void ProcessNoMod(double* input, double* output, int sampleCount)
		{
			// [Spatial Echo] Backstop clamp, matching the one in Update(). This
			// path used SampleDelay raw, so a request longer than the buffer
			// silently WRAPPED (realising SampleDelay modulo the capacity), and
			// a request of twice the capacity or more read out of bounds. With
			// capacity now derived from the engine sample rate this cannot
			// engage in normal operation.
			int effectiveDelay = SampleDelay;
			if (effectiveDelay > DelayBufferSize - 2) effectiveDelay = DelayBufferSize - 2;
			if (effectiveDelay < 0) effectiveDelay = 0;
			realisedDelay = (double)effectiveDelay;

			auto delayedIndex = index - effectiveDelay;
			if (delayedIndex < 0) delayedIndex += DelayBufferSize;
			const bool enableDrive = Drive > 0.0001;

			for (int i = 0; i < sampleCount; i++)
			{
				auto bufOut = delayBuffer[delayedIndex];
				auto inVal = input[i] + bufOut * Feedback;
				if (enableDrive) inVal = SoftClip(inVal, Drive);

				delayBuffer[index] = inVal;
				output[i] = bufOut - inVal * Feedback;

				index++;
				delayedIndex++;
				if (index >= DelayBufferSize) index -= DelayBufferSize;
				if (delayedIndex >= DelayBufferSize) delayedIndex -= DelayBufferSize;
				samplesProcessed++;
			}
		}

		void ProcessWithMod(double* input, double* output, int sampleCount)
		{
			const bool enableDrive = Drive > 0.0001;
			for (int i = 0; i < sampleCount; i++)
			{
				if (samplesProcessed >= ModulationUpdateRate)
					Update();

				double bufOut;

				if (InterpolationEnabled)
				{
					int idxA = index - delayA;
					int idxB = index - delayB;
					idxA += DelayBufferSize * (idxA < 0);
					idxB += DelayBufferSize * (idxB < 0);

					bufOut = delayBuffer[idxA] * gainA + delayBuffer[idxB] * gainB;
				}
				else
				{
					int idxA = index - delayA;
					idxA += DelayBufferSize * (idxA < 0);
					bufOut = delayBuffer[idxA];
				}

				auto inVal = input[i] + bufOut * Feedback;
				if (enableDrive) inVal = SoftClip(inVal, Drive);
				delayBuffer[index] = inVal;
				output[i] = bufOut - inVal * Feedback;

				index++;
				if (index >= DelayBufferSize) index -= DelayBufferSize;
				samplesProcessed++;
			}
		}

		inline double Get(int delay)
		{
			int idx = index - delay;
			if (idx < 0)
				idx += DelayBufferSize;

			return delayBuffer[idx];
		}

		void Update()
		{
			modPhase += ModRate * ModulationUpdateRate;
			if (modPhase > 1)
				modPhase = std::fmod(modPhase, 1.0);

			// [vendored] std::sinf -> std::sin
			auto mod = std::sin(modPhase * 2 * M_PI);

			if (ModAmount >= SampleDelay)
				ModAmount = SampleDelay - 1;

			auto totalDelay = SampleDelay + ModAmount * mod;

			// [Spatial Echo] Backstop clamp. The buffer is now sized from the
			// engine sample rate for the longest delay the vendored parameter
			// laws can request, so this cannot engage in normal operation; it
			// remains so an out-of-range caller still cannot read out of bounds.
			if (totalDelay > (double)(DelayBufferSize - 2))
				totalDelay = (double)(DelayBufferSize - 2);

			if (totalDelay <= 0)
				totalDelay = 1;

			realisedDelay = totalDelay;

			delayA = (int)totalDelay;
			delayB = (int)totalDelay + 1;

			auto partial = totalDelay - delayA;

			gainA = 1.0 - partial;
			gainB = partial;

			samplesProcessed = 0;
		}
	};
}
