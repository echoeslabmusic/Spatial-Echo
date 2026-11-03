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

#include <array>
#include "ModulatedAllpass.h"
#include "RandomBuffer.h"

namespace Cloudseed
{
	class AllpassDiffuser
	{
	public:
		static const int MaxStageCount = 12;

	private:
		int samplerate = 48000;

		ModulatedAllpass filters[MaxStageCount];
		// Match ModulatedAllpass' safe construction defaults. UpdateSeeds() and
		// SetSamplerate() both consume these values from this constructor, before
		// any host or preset parameter replay can reach the diffuser.
		int delay = 100;
		float modRate = 0.0f;
		std::array<float, MaxStageCount * 3> seedValues{};
		int seed;
		float crossSeed;

	public:
		int Stages;

		AllpassDiffuser()
		{
			crossSeed = 0.0;
			seed = 23456;
			UpdateSeeds();
			Stages = 1;

			SetSamplerate(48000);
		}

		int GetSamplerate()
		{
			return samplerate;
		}

		void SetSamplerate(int samplerate)
		{
			this->samplerate = samplerate;
			// [Spatial Echo] Prepare-time only; this allocates. Every stage gets
			// enough room for the longest delay the vendored parameter law can
			// request at this rate, so the stage delays are no longer wrapped or
			// clamped at high sample rates.
			const int capacity = Capacity::AllpassSamples(samplerate);
			for (int i = 0; i < MaxStageCount; i++)
				filters[i].SetCapacity(capacity);
			SetModRate(modRate);
			Update();
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


		bool GetModulationEnabled()
		{
			return filters[0].ModulationEnabled;
		}

		void SetModulationEnabled(bool value)
		{
			for (int i = 0; i < MaxStageCount; i++)
				filters[i].ModulationEnabled = value;

		}

		void SetInterpolationEnabled(bool enabled)
		{
			for (int i = 0; i < MaxStageCount; i++)
				filters[i].InterpolationEnabled = enabled;
		}

		void SetDelay(int delaySamples)
		{
			delay = delaySamples;
			Update();
		}

		void SetFeedback(double feedback)
		{
			for (int i = 0; i < MaxStageCount; i++)
				filters[i].Feedback = feedback;
		}

		void SetDrive(double drive)
		{
			for (int i = 0; i < MaxStageCount; i++)
				filters[i].Drive = drive;
		}

		void SetModAmount(double amount)
		{
			for (int i = 0; i < MaxStageCount; i++)
				filters[i].ModAmount = amount * (0.85 + 0.3 * seedValues[MaxStageCount + i]);
		}

		void SetModRate(double rate)
		{
			modRate = rate;

			for (int i = 0; i < MaxStageCount; i++)
				filters[i].ModRate = rate * (0.85 + 0.3 * seedValues[MaxStageCount * 2 + i]) / samplerate;
		}

		// [Spatial Echo] Deterministic modulation phase for one stage. Assigned
		// by the owner at prepare time; see ReverbChannel::SeedModulationPhases.
		void SetStageModulationPhase(int stage, double value01)
		{
			if (stage < 0 || stage >= MaxStageCount)
				return;
			filters[stage].SetModulationPhase(value01);
		}

		// [Spatial Echo] Number of stages that actually process audio. Stages at
		// or above this index never run, so their cached realised delay is stale
		// by design and must not be compared against their request.
		int GetActiveStageCount() const
		{
			return Stages < 1 ? 1 : (Stages > MaxStageCount ? MaxStageCount : Stages);
		}

		// [Spatial Echo] Read-only capacity / delay introspection, used by the
		// ReverbFractionalDelayReader gate to prove that every stage realises
		// the delay it was asked for at every supported sample rate.
		int GetStageCapacity(int stage) const
		{
			return filters[stage < 0 ? 0 : (stage >= MaxStageCount ? MaxStageCount - 1 : stage)]
				.GetCapacity();
		}

		int GetStageRequestedDelaySamples(int stage) const
		{
			return filters[stage < 0 ? 0 : (stage >= MaxStageCount ? MaxStageCount - 1 : stage)]
				.GetRequestedDelaySamples();
		}

		double GetStageRealisedDelaySamples(int stage) const
		{
			return filters[stage < 0 ? 0 : (stage >= MaxStageCount ? MaxStageCount - 1 : stage)]
				.GetRealisedDelaySamples();
		}

		size_t GetAllocatedBytes() const
		{
			size_t total = 0;
			for (int i = 0; i < MaxStageCount; i++)
				total += filters[i].GetAllocatedBytes();
			return total;
		}

		// Total delay this diffuser contributes to whatever loop it sits in.
		// The late lines put the diffuser INSIDE their feedback loop, so the
		// Jot per-line gain has to be computed over delay + this, not over the
		// delay line alone.
		int GetTotalDelaySamples() const
		{
			const int active = Stages < 1 ? 1 : (Stages > MaxStageCount ? MaxStageCount : Stages);
			int total = 0;
			for (int i = 0; i < active; i++)
				total += filters[i].SampleDelay;
			return total;
		}

		void Process(double* input, double* output, int bufSize)
		{
			double tempBuffer[BUFFER_SIZE];

			filters[0].Process(input, tempBuffer, bufSize);

			for (int i = 1; i < Stages; i++)
				filters[i].Process(tempBuffer, tempBuffer, bufSize);
			
			Utils::Copy(output, tempBuffer, bufSize);
		}

		void ClearBuffers()
		{
			for (int i = 0; i < MaxStageCount; i++)
				filters[i].ClearBuffers();
		}

	private:
		void Update()
		{
			for (int i = 0; i < MaxStageCount; i++)
			{
				auto r = seedValues[i];
				auto d = std::pow(10, r) * 0.1; // 0.1 ... 1.0
				filters[i].SampleDelay = (int)(delay * d);
			}
		}

		void UpdateSeeds()
		{
			RandomBuffer::Fill(static_cast<uint64_t>(seed),
			                   static_cast<int>(seedValues.size()),
			                   crossSeed,
			                   seedValues.data());
			Update();
		}

	};
}
