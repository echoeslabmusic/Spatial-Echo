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

#include "Lp1.h"
#include "ModulatedDelay.h"
#include "AllpassDiffuser.h"
#include "Biquad.h"
#include <cmath>

namespace Cloudseed
{
	class DelayLine
	{
	private:
		ModulatedDelay delay;
		AllpassDiffuser diffuser;
		Biquad lowShelf;
		Biquad highShelf;
		Lp1 lowPass;
		double feedback;
		double feedbackDrive;

		static inline double SoftClip(double x, double drive)
		{
			x = x * drive;
			if (x > 1.0) x = 1.0;
			else if (x < -1.0) x = -1.0;
			return (x - x * x * x / 3.0) / drive;
		}

	public:
		bool DiffuserEnabled = false;
		bool LowShelfEnabled = false;
		bool HighShelfEnabled = false;
		bool CutoffEnabled = false;
		bool TapPostDiffuser = false;

		DelayLine() :
			lowShelf(Biquad::FilterType::LowShelf, 48000),
			highShelf(Biquad::FilterType::HighShelf, 48000)
		{
			feedback = 0;
			feedbackDrive = 0.0;

			lowShelf.SetGainDb(-20);
			lowShelf.Frequency = 20;

			highShelf.SetGainDb(-20);
			highShelf.Frequency = 19000;

			lowPass.SetCutoffHz(1000);
			lowShelf.Update();
			highShelf.Update();
			SetSamplerate(48000);
			SetDiffuserSeed(1, 0.0);
		}

		void SetSamplerate(int samplerate)
		{
			// [Spatial Echo] Prepare-time only; this allocates. The late line
			// delay is LateLineSize (up to 1000 ms) scaled by (0.5 + seed) in
			// ReverbChannel::UpdateLines, so it needs 1.5 s at this rate plus the
			// modulation excursion. Before this it was a fixed 384000 samples,
			// which is only 1.0 s once a 192 kHz host drives the engine at
			// 384 kHz -- see Cloudseed::Capacity in Utils.h.
			delay.SetCapacity(Capacity::LateLineSamples(samplerate));
			diffuser.SetSamplerate(samplerate);
			lowPass.SetSamplerate((double)samplerate);
			lowShelf.SetSamplerate((double)samplerate);
			highShelf.SetSamplerate((double)samplerate);
		}

		// [Spatial Echo] Deterministic modulation phases, assigned by the owner
		// at prepare time; see ReverbChannel::SeedModulationPhases.
		void SetLineModulationPhase(double value01)
		{
			delay.SetModulationPhase(value01);
		}

		void SetDiffuserStageModulationPhase(int stage, double value01)
		{
			diffuser.SetStageModulationPhase(stage, value01);
		}

		// [Spatial Echo] Read-only introspection for the delay-realisation gate.
		int GetLineCapacity() const { return delay.GetCapacity(); }
		int GetLineRequestedDelaySamples() const { return delay.GetRequestedDelaySamples(); }
		double GetLineRealisedDelaySamples() const { return delay.GetRealisedDelaySamples(); }
		int GetDiffuserActiveStageCount() const { return diffuser.GetActiveStageCount(); }
		int GetDiffuserStageCapacity(int stage) const { return diffuser.GetStageCapacity(stage); }
		int GetDiffuserStageRequestedDelaySamples(int stage) const
		{ return diffuser.GetStageRequestedDelaySamples(stage); }
		double GetDiffuserStageRealisedDelaySamples(int stage) const
		{ return diffuser.GetStageRealisedDelaySamples(stage); }
		size_t GetAllocatedBytes() const
		{
			return delay.GetAllocatedBytes() + diffuser.GetAllocatedBytes();
		}

		void SetDiffuserSeed(int seed, double crossSeed)
		{
			diffuser.SetSeed(seed);
			diffuser.SetCrossSeed(crossSeed);
		}

		void SetDelay(int delaySamples)
		{
			delay.SampleDelay = delaySamples;
		}

		void SetFeedback(double feedb)
		{
			feedback = feedb;
		}

		double GetFeedback() const { return feedback; }

		void SetDiffuserDelay(int delaySamples)
		{
			diffuser.SetDelay(delaySamples);
		}

		void SetDiffuserFeedback(double feedb)
		{
			diffuser.SetFeedback(feedb);
		}

		void SetDiffuserStages(int stages)
		{
			diffuser.Stages = stages;
		}

		// Extra loop delay contributed by the post-diffuser. Both branches of
		// Process() keep the diffuser inside the feedback path; only the tap
		// point differs. Zero when the diffuser is bypassed.
		int GetDiffuserDelaySamples() const
		{
			return DiffuserEnabled ? diffuser.GetTotalDelaySamples() : 0;
		}

		void SetLowShelfGain(double gainDb)
		{
			lowShelf.SetGainDb(gainDb);
			lowShelf.Update();
		}

		void SetLowShelfFrequency(double frequency)
		{
			lowShelf.Frequency = frequency;
			lowShelf.Update();
		}

		void SetHighShelfGain(double gainDb)
		{
			highShelf.SetGainDb(gainDb);
			highShelf.Update();
		}

		void SetHighShelfFrequency(double frequency)
		{
			highShelf.Frequency = frequency;
			highShelf.Update();
		}

		void SetCutoffFrequency(double frequency)
		{
			lowPass.SetCutoffHz(frequency);
		}

		void SetLineModAmount(double amount)
		{
			delay.ModAmount = amount;
		}

		void SetLineModRate(double rate)
		{
			delay.ModRate = rate;
		}

		void SetDiffuserModAmount(double amount)
		{
			diffuser.SetModulationEnabled(amount > 0.0);
			diffuser.SetModAmount(amount);
		}

		void SetDiffuserModRate(double rate)
		{
			diffuser.SetModRate(rate);
		}

		void SetInterpolationEnabled(bool value)
		{
			diffuser.SetInterpolationEnabled(value);
		}

		void SetDiffuserDrive(double drive)
		{
			diffuser.SetDrive(drive);
		}

		void SetFeedbackDrive(double drive)
		{
			feedbackDrive = drive;
		}

		void Process(double* input, double* output, int bufSize)
		{
			double tempBuffer[BUFFER_SIZE];
			double delayedOutput[BUFFER_SIZE];
			const bool enableFbDrive = feedbackDrive > 0.0001;

			while (bufSize > 0)
			{
				// Read the physical delay before computing its next inputs. The
				// bound guarantees that neither interpolation tap needs a sample
				// from this chunk, even at the largest modulation excursion.
				// The former Pop/Push queue added one host-dependent block to
				// every feedback iteration, changing decay and modal spacing.
				const int count = std::min({ bufSize, BUFFER_SIZE, delay.GetFeedbackReadLimit() });
				delay.ReadFeedback(tempBuffer, count);
				if (!TapPostDiffuser)
				{
					Utils::Copy(delayedOutput, tempBuffer, count);
					if (DiffuserEnabled)
						diffuser.Process(tempBuffer, tempBuffer, count);
				}

				if (LowShelfEnabled)
					lowShelf.Process(tempBuffer, tempBuffer, count);
				if (HighShelfEnabled)
					highShelf.Process(tempBuffer, tempBuffer, count);
				if (CutoffEnabled)
					lowPass.Process(tempBuffer, tempBuffer, count);

				for (int i = 0; i < count; i++)
				{
					tempBuffer[i] = input[i] + tempBuffer[i] * feedback;
					if (enableFbDrive)
						tempBuffer[i] = SoftClip(tempBuffer[i], feedbackDrive);
				}

				if (TapPostDiffuser && DiffuserEnabled)
					diffuser.Process(tempBuffer, tempBuffer, count);
				delay.WriteFeedback(tempBuffer, count);
				Utils::Copy(output, TapPostDiffuser ? tempBuffer : delayedOutput, count);
				input += count;
				output += count;
				bufSize -= count;
			}
		}

		void ClearDiffuserBuffer()
		{
			diffuser.ClearBuffers();
		}

		void ClearBuffers()
		{
			delay.ClearBuffers();
			diffuser.ClearBuffers();
			lowShelf.ClearBuffers();
			highShelf.ClearBuffers();
			lowPass.Output = 0;
		}
	};
}
