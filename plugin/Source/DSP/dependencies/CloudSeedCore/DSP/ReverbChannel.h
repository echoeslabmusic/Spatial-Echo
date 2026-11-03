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

#include <map>
#include <memory>
#include <array>
#include "../Parameters.h"
#include "ModulatedDelay.h"
#include "MultitapDelay.h"
#include "RandomBuffer.h"
#include "Lp1.h"
#include "Hp1.h"
#include "DelayLine.h"
#include "AllpassDiffuser.h"
#include <cmath>
// [vendored] self-include removed
#include "Utils.h"
// [Spatial Echo] Deterministic modulation-phase seeding. PCG32 is a
// dependency-free header-only generator that already ships in this codebase; it
// replaces the process-global std::rand() the modulated elements used to seed
// their phase from.
#include "../../../PCG32.h"

namespace Cloudseed
{
	enum class ChannelLR
	{
		Left,
		Right
	};

	class ReverbChannel
	{
	private:
		static const int TotalLineCount = 12;

		double paramsScaled[Parameter::COUNT] = { 0.0 };
		bool parameterInitialized[Parameter::COUNT] = { false };
		int samplerate;

		ModulatedDelay preDelay;
		MultitapDelay multitap;
		AllpassDiffuser diffuser;
		DelayLine lines[TotalLineCount];
		Hp1 highPass;
		Lp1 lowPass;
		Lp1 earlyOutLowPass;
		bool earlyOutLpEnabled = false;

		int delayLineSeed = 0;
		int postDiffusionSeed = 0;

		// Used the the main process loop
		int lineCount = 8;

		bool lowCutEnabled = false;
		bool highCutEnabled = false;
		bool multitapEnabled = false;
		bool diffuserEnabled = false;
		double inputMix = 0.0;
		double dryOut = 0.0;
		double earlyOut = 0.0;
		double lineOut = 0.0;
		double crossSeed = 0.0;
		ChannelLR channelLr = ChannelLR::Left;

	public:

		ReverbChannel(int samplerate, ChannelLR leftOrRight)
		{
			this->channelLr = leftOrRight;
			crossSeed = 0.0;
			lineCount = 8;
			diffuser.SetInterpolationEnabled(true);
			diffuser.SetDrive(2.0);
			for (int i = 0; i < TotalLineCount; i++)
				lines[i].SetDiffuserDrive(2.0);
			highPass.SetCutoffHz(20);
			lowPass.SetCutoffHz(20000);
			earlyOutLowPass.SetCutoffHz(20000);
			earlyOutLpEnabled = false;
			SetSamplerate(samplerate);
		}

		int GetSamplerate()
		{
			return samplerate;
		}

		void SetSamplerate(int samplerate)
		{
			this->samplerate = samplerate;
			highPass.SetSamplerate((double)samplerate);
			lowPass.SetSamplerate((double)samplerate);
			earlyOutLowPass.SetSamplerate((double)samplerate);

			// [Spatial Echo] Prepare-time only; these allocate. Every delay
			// buffer in the channel is sized here from the engine sample rate for
			// the longest delay its parameter law can request, replacing the
			// compile-time worst-case arrays. Nothing below this point, and
			// nothing on the audio thread, resizes anything.
			preDelay.SetCapacity(Capacity::PreDelaySamples(samplerate));
			multitap.SetCapacity(Capacity::MultitapSamples(samplerate));
			diffuser.SetSamplerate(samplerate);

			for (int i = 0; i < TotalLineCount; i++)
				lines[i].SetSamplerate(samplerate);

			SeedModulationPhases();

			ReapplyAllParams();
			ClearBuffers();
			UpdateLines();
		}

		// [Spatial Echo] Total heap held by this channel's delay buffers.
		size_t GetAllocatedBytes() const
		{
			size_t total = preDelay.GetAllocatedBytes()
			             + multitap.GetAllocatedBytes()
			             + diffuser.GetAllocatedBytes();
			for (int i = 0; i < TotalLineCount; i++)
				total += lines[i].GetAllocatedBytes();
			return total;
		}

		// [Spatial Echo] Read-only introspection for the delay-realisation gate.
		static int GetTotalLineCount() { return TotalLineCount; }
		int GetActiveLineCount() const { return lineCount; }
		const DelayLine& GetLine(int i) const
		{
			return lines[i < 0 ? 0 : (i >= TotalLineCount ? TotalLineCount - 1 : i)];
		}
		const AllpassDiffuser& GetEarlyDiffuser() const { return diffuser; }
		int GetPreDelayCapacity() const { return preDelay.GetCapacity(); }
		int GetPreDelayRequestedSamples() const { return preDelay.GetRequestedDelaySamples(); }
		double GetPreDelayRealisedSamples() const { return preDelay.GetRealisedDelaySamples(); }
		int GetMultitapCapacity() const { return multitap.GetCapacity(); }
		double GetMultitapRealisedLengthSamples() const
		{ return multitap.GetRealisedTapLengthSamples(); }

		void ReapplyAllParams()
		{
			for (int i = 0; i < Parameter::COUNT; i++)
				parameterInitialized[i] = false;

			for (int i = 0; i < Parameter::COUNT; i++)
				SetParameter(i, paramsScaled[i]);
		}

		void SetParameter(int para, double scaledValue)
		{
			const bool valueChanged = !parameterInitialized[para] || paramsScaled[para] != scaledValue;
			paramsScaled[para] = scaledValue;
			parameterInitialized[para] = true;

			switch (para)
			{
			case Parameter::Interpolation:
				for (int i = 0; i < TotalLineCount; i++)
					lines[i].SetInterpolationEnabled(scaledValue >= 0.5);
				break;
			case Parameter::LowCutEnabled:
			{
				const bool newLowCutEnabled = scaledValue >= 0.5;
				if (!lowCutEnabled && newLowCutEnabled)
					highPass.ClearBuffers();
				lowCutEnabled = newLowCutEnabled;
				break;
			}
			case Parameter::HighCutEnabled:
			{
				const bool newHighCutEnabled = scaledValue >= 0.5;
				if (!highCutEnabled && newHighCutEnabled)
					lowPass.ClearBuffers();
				highCutEnabled = newHighCutEnabled;
				break;
			}
			case Parameter::InputMix:
				inputMix = scaledValue;
				break;
			case Parameter::LowCut:
				highPass.SetCutoffHz(scaledValue);
				break;
			case Parameter::HighCut:
				lowPass.SetCutoffHz(scaledValue);
				break;
			case Parameter::DryOut:
				dryOut = scaledValue <= -30 ? 0.0 : Utils::DB2Gain(scaledValue);
				break;
			case Parameter::EarlyOut:
				earlyOut = scaledValue <= -30 ? 0.0 : Utils::DB2Gain(scaledValue);
				break;
			case Parameter::LateOut:
				lineOut = scaledValue <= -30 ? 0.0 : Utils::DB2Gain(scaledValue);
				break;


			case Parameter::TapEnabled:
			{
				auto newVal = scaledValue >= 0.5;
				if (newVal != multitapEnabled)
					multitap.ClearBuffers();
				multitapEnabled = newVal;
				break;
			}
			case Parameter::TapCount:
				if (!valueChanged)
					break;
				multitap.SetTapCount((int)scaledValue);
				break;
			case Parameter::TapDecay:
				multitap.SetTapDecay(scaledValue);
				break;
			case Parameter::TapPredelay:
				preDelay.SampleDelay = (int)Ms2Samples(scaledValue);
				break;
			case Parameter::TapLength:
				if (!valueChanged)
					break;
				multitap.SetTapLength((int)Ms2Samples(scaledValue));
				break;


			case Parameter::EarlyDiffuseEnabled:
			{
				auto newVal = scaledValue >= 0.5;
				if (newVal != diffuserEnabled)
					diffuser.ClearBuffers();
				diffuserEnabled = newVal;
				break;
			}
			case Parameter::EarlyDiffuseCount:
				diffuser.Stages = (int)scaledValue;
				break;
			case Parameter::EarlyDiffuseDelay:
				if (!valueChanged)
					break;
				diffuser.SetDelay((int)Ms2Samples(scaledValue));
				break;
			case Parameter::EarlyDiffuseModAmount:
				diffuser.SetModulationEnabled(scaledValue > 0.5);
				diffuser.SetModAmount(Ms2Samples(scaledValue));
				break;
			case Parameter::EarlyDiffuseFeedback:
				diffuser.SetFeedback(scaledValue);
				break;
			case Parameter::EarlyDiffuseModRate:
				diffuser.SetModRate(scaledValue);
				break;


			case Parameter::LateMode:
				for (int i = 0; i < TotalLineCount; i++)
					lines[i].TapPostDiffuser = scaledValue >= 0.5;
				break;
			case Parameter::LateLineCount:
				lineCount = (int)scaledValue;
				break;
			case Parameter::LateDiffuseEnabled:
				if (!valueChanged)
					break;
				for (int i = 0; i < TotalLineCount; i++)
				{
					auto newVal = scaledValue >= 0.5;
					if (newVal != lines[i].DiffuserEnabled)
						lines[i].ClearDiffuserBuffer();
					lines[i].DiffuserEnabled = newVal;
				}
				UpdateLines();
				break;
			case Parameter::LateDiffuseCount:
				// Guarded like every other structural case. Re-applying an
				// unchanged value must be a no-op: CloudFilterStateContinuity
				// renders the same parameters one-shot and again per block and
				// requires bit-comparable output, and UpdateLines() re-derives
				// every line's delay and feedback from scratch.
				if (!valueChanged)
					break;
				for (int i = 0; i < TotalLineCount; i++)
					lines[i].SetDiffuserStages((int)scaledValue);
				// Stage count changes the in-loop diffuser delay, which the
				// per-line feedback gain is derived from.
				UpdateLines();
				break;
			case Parameter::LateLineSize:
				if (!valueChanged)
					break;
				UpdateLines();
				break;
			case Parameter::LateLineModAmount:
				if (!valueChanged)
					break;
				UpdateLines();
				break;
			case Parameter::LateDiffuseDelay:
				if (!valueChanged)
					break;
				for (int i = 0; i < TotalLineCount; i++)
					lines[i].SetDiffuserDelay((int)Ms2Samples(scaledValue));
				// Same reason as LateDiffuseCount: this is in-loop delay.
				UpdateLines();
				break;
			case Parameter::LateDiffuseModAmount:
				if (!valueChanged)
					break;
				UpdateLines();
				break;
			case Parameter::LateLineDecay:
				if (!valueChanged)
					break;
				UpdateLines();
				break;
			case Parameter::LateLineModRate:
				if (!valueChanged)
					break;
				UpdateLines();
				break;
			case Parameter::LateDiffuseFeedback:
				for (int i = 0; i < TotalLineCount; i++)
					lines[i].SetDiffuserFeedback(scaledValue);
				break;
			case Parameter::LateDiffuseModRate:
				if (!valueChanged)
					break;
				UpdateLines();
				break;


			case Parameter::EqLowShelfEnabled:
				for (int i = 0; i < TotalLineCount; i++)
					lines[i].LowShelfEnabled = scaledValue >= 0.5;
				break;
			case Parameter::EqHighShelfEnabled:
				for (int i = 0; i < TotalLineCount; i++)
					lines[i].HighShelfEnabled = scaledValue >= 0.5;
				break;
			case Parameter::EqLowpassEnabled:
				earlyOutLpEnabled = scaledValue >= 0.5;
				for (int i = 0; i < TotalLineCount; i++)
					lines[i].CutoffEnabled = scaledValue >= 0.5;
				break;
			case Parameter::EqLowFreq:
				for (int i = 0; i < TotalLineCount; i++)
					lines[i].SetLowShelfFrequency(scaledValue);
				break;
			case Parameter::EqHighFreq:
				for (int i = 0; i < TotalLineCount; i++)
					lines[i].SetHighShelfFrequency(scaledValue);
				break;
			case Parameter::EqCutoff:
				earlyOutLowPass.SetCutoffHz(scaledValue);
				for (int i = 0; i < TotalLineCount; i++)
					lines[i].SetCutoffFrequency(scaledValue);
				break;
			case Parameter::EqLowGain:
				for (int i = 0; i < TotalLineCount; i++)
					lines[i].SetLowShelfGain(scaledValue);
				break;
			case Parameter::EqHighGain:
				for (int i = 0; i < TotalLineCount; i++)
					lines[i].SetHighShelfGain(scaledValue);
				break;


			case Parameter::EqCrossSeed:
				if (!valueChanged)
					break;
				crossSeed = channelLr == ChannelLR::Right ? 0.5 * scaledValue : 1.0 - 0.5 * scaledValue;
				multitap.SetCrossSeed(crossSeed);
				diffuser.SetCrossSeed(crossSeed);
				UpdatePostDiffusion();
				break;


			case Parameter::SeedTap:
				if (!valueChanged)
					break;
				multitap.SetSeed((int)scaledValue);
				break;
			case Parameter::SeedDiffusion:
				if (!valueChanged)
					break;
				diffuser.SetSeed((int)scaledValue);
				break;
			case Parameter::SeedDelay:
				if (!valueChanged)
					break;
				delayLineSeed = (int)scaledValue;
				UpdateLines();
				break;
			case Parameter::SeedPostDiffusion:
				if (!valueChanged)
					break;
				postDiffusionSeed = (int)scaledValue;
				UpdatePostDiffusion();
				break;
			}
		}

		void Process(float* input, float* output, int bufSize)
		{
			double tempBuffer[BUFFER_SIZE];
			double earlyOutBuffer[BUFFER_SIZE];
			double lineOutBuffer[BUFFER_SIZE];
			double lineSumBuffer[BUFFER_SIZE];

			// float -> double at input boundary
			for (int i = 0; i < bufSize; i++)
				tempBuffer[i] = (double)input[i];

			if (lowCutEnabled)
				highPass.Process(tempBuffer, tempBuffer, bufSize);
			if (highCutEnabled)
				lowPass.Process(tempBuffer, tempBuffer, bufSize);

			// completely zero if no input present
			// Previously, the very small values were causing some really strange CPU spikes
			for (int i = 0; i < bufSize; i++)
			{
				auto n = tempBuffer[i];
				if (n * n < 0.000000000000001)
					tempBuffer[i] = 0.0;
			}

			preDelay.Process(tempBuffer, tempBuffer, bufSize);
			if (multitapEnabled)
				multitap.Process(tempBuffer, tempBuffer, bufSize);
			if (diffuserEnabled)
				diffuser.Process(tempBuffer, tempBuffer, bufSize);

			Utils::Copy(earlyOutBuffer, tempBuffer, bufSize);
			if (earlyOutLpEnabled)
				earlyOutLowPass.Process(earlyOutBuffer, earlyOutBuffer, bufSize);
			Utils::ZeroBuffer(lineSumBuffer, bufSize);
			for (int i = 0; i < lineCount; i++)
			{
				lines[i].Process(tempBuffer, lineOutBuffer, bufSize);
				Utils::Mix(lineSumBuffer, lineOutBuffer, 1.0, bufSize);
			}

			auto perLineGain = GetPerLineGain();
			Utils::Gain(lineSumBuffer, (double)perLineGain, bufSize);

			// double -> float at output boundary
			for (int i = 0; i < bufSize; i++)
			{
				output[i] = (float)(dryOut * (double)input[i]
					+ earlyOut * earlyOutBuffer[i]
					+ lineOut * lineSumBuffer[i]);
			}
		}

		void ClearBuffers()
		{
			lowPass.ClearBuffers();
			highPass.ClearBuffers();
			earlyOutLowPass.ClearBuffers();
			preDelay.ClearBuffers();
			multitap.ClearBuffers();
			diffuser.ClearBuffers();
			for (int i = 0; i < TotalLineCount; i++)
				lines[i].ClearBuffers();
		}


	private:
		// [Spatial Echo] Assign every modulated element a deterministic phase.
		//
		// Cloudseed::ModulatedDelay and Cloudseed::ModulatedAllpass used to seed
		// their modulation phase in their constructors from the process-global
		// std::rand(). That made the reverb's SOUND a function of global PRNG
		// state: it only sounded stable because nothing upstream happened to
		// consume rand(), which is why several reverb tests called std::srand()
		// before constructing an engine. Any future upstream rand() user would
		// have made the reverb genuinely nondeterministic.
		//
		// Phases now come from a PCG32 stream seeded from a fixed constant plus
		// the channel identity, drawn in a fixed order. The L and R tanks stay
		// decorrelated (different streams), the result is reproducible across
		// processes and independent of global state, and - as before - phases are
		// assigned once per prepare and never touched again, so no parameter
		// change can introduce a phase discontinuity mid-stream.
		void SeedModulationPhases()
		{
			PCG32 rng;
			rng.seed(0x5EED'C10D'2024'0001ull,
			         channelLr == ChannelLR::Left ? 0x9E3779B97F4A7C15ull
			                                      : 0xC2B2AE3D27D4EB4Full);

			preDelay.SetModulationPhase((double)rng.nextFloat());

			for (int stage = 0; stage < AllpassDiffuser::MaxStageCount; stage++)
				diffuser.SetStageModulationPhase(stage, (double)rng.nextFloat());

			for (int i = 0; i < TotalLineCount; i++)
			{
				lines[i].SetLineModulationPhase((double)rng.nextFloat());
				for (int stage = 0; stage < AllpassDiffuser::MaxStageCount; stage++)
					lines[i].SetDiffuserStageModulationPhase(stage, (double)rng.nextFloat());
			}
		}

		double GetPerLineGain()
		{
			return 1.0 / std::sqrt((double)lineCount);
		}

		void UpdateLines()
		{
			auto lineDelaySamples = (int)Ms2Samples(paramsScaled[Parameter::LateLineSize]);
			auto lineDecayMillis = paramsScaled[Parameter::LateLineDecay] * 1000.0;
			auto lineDecaySamples = Ms2Samples(lineDecayMillis);

			auto lineModAmount = Ms2Samples(paramsScaled[Parameter::LateLineModAmount]);
			auto lineModRate = paramsScaled[Parameter::LateLineModRate];

			auto lateDiffusionModAmount = Ms2Samples(paramsScaled[Parameter::LateDiffuseModAmount]);
			auto lateDiffusionModRate = paramsScaled[Parameter::LateDiffuseModRate];

			std::array<float, TotalLineCount * 3> delayLineSeeds{};
			RandomBuffer::Fill(static_cast<uint64_t>(delayLineSeed),
			                   static_cast<int>(delayLineSeeds.size()),
			                   static_cast<float>(crossSeed),
			                   delayLineSeeds.data());

			for (int i = 0; i < TotalLineCount; i++)
			{
				auto modAmount = lineModAmount * (0.7 + 0.3 * delayLineSeeds[i + TotalLineCount]);
				auto modRate = lineModRate * (0.7 + 0.3 * delayLineSeeds[i + 2 * TotalLineCount]) / samplerate;

				auto delaySamples = (0.5 + 1.0 * delayLineSeeds[i]) * lineDelaySamples;
				if (delaySamples < modAmount + 2) // when the delay is set really short, and the modulation is very high
					delaySamples = modAmount + 2; // the mod could actually take the delay time negative, prevent that! -- provide 2 extra sample as margin of safety

				// Average loop-time decay estimate, including diffuser delays.
				// A Schroeder allpass has frequency-dependent group delay, so
				// summing its storage lengths is an average, not an exact T60
				// guarantee. Exact decay control in the static linear prototype
				// would put length-proportional absorption in EVERY delay,
				// including each allpass (Dahl/Jot, DAFx-2000, section 2.2):
				// https://www.dafx.de/paper-archive/2000/pdf/Dahl_Final.pdf
				// Retain this voicing approximation rather than compensate for
				// the removed, host-dependent feedback queue with another gain.
				//
				// Omitting the in-loop diffuser's mean delay underestimates the
				// loss needed per return, especially in compact tanks where the
				// diffuser lengths can dominate the main delay.
				lines[i].SetDelay((int)delaySamples);
				const double loopDelaySamples = delaySamples
					+ (double)lines[i].GetDiffuserDelaySamples();
				auto dbAfter1Iteration = loopDelaySamples / lineDecaySamples * (-60.0); // lineDecay is the time it takes to reach T60
				auto gainAfter1Iteration = Utils::DB2Gain(dbAfter1Iteration);

				lines[i].SetFeedback(gainAfter1Iteration);
				lines[i].SetLineModAmount(modAmount);
				lines[i].SetLineModRate(modRate);
				lines[i].SetDiffuserModAmount(lateDiffusionModAmount);
				lines[i].SetDiffuserModRate(lateDiffusionModRate);
			}
		}

		void UpdatePostDiffusion()
		{
			for (int i = 0; i < TotalLineCount; i++)
				lines[i].SetDiffuserSeed((postDiffusionSeed) * (i + 1), crossSeed);
			// The seed changes the in-loop diffuser lengths. Compute feedback
			// only after the final geometry is installed, including width edits.
			UpdateLines();
		}

		double Ms2Samples(double value)
		{
			return value / 1000.0 * samplerate;
		}

	};
}
