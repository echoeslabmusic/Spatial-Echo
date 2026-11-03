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

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <cmath>

namespace Cloudseed
{
	class Lp1
	{
	private:
		double fs;
		double b0, a1;
		double cutoffHz;

	public:
		double Output;

		Lp1()
		{
			fs = 48000.0;
			b0 = 1.0;
			a1 = 0.0;
			cutoffHz = 1000.0;
			Output = 0.0;
		}

		double GetSamplerate()
		{
			return fs;
		}

		void SetSamplerate(double samplerate)
		{
			fs = samplerate;
		}

		double GetCutoffHz()
		{
			return cutoffHz;
		}

		void SetCutoffHz(double hz)
		{
			cutoffHz = hz;
			Update();
		}

		void ClearBuffers()
		{
			Output = 0;
		}

		void Update()
		{
			// Prevent going over the Nyquist frequency
			if (cutoffHz >= fs * 0.5)
				cutoffHz = fs * 0.499;

			auto x = 2.0 * M_PI * cutoffHz / fs;
			auto nn = (2.0 - std::cos(x));
			auto alpha = nn - std::sqrt(nn * nn - 1.0);

			a1 = alpha;
			b0 = 1.0 - alpha;
		}

		double Process(double input)
		{
			if (input == 0.0 && std::abs(Output) < 0.000000000001)
			{
				Output = 0.0;
			}
			else
			{
				Output = b0 * input + a1 * Output;
			}
			return Output;
		}

		void Process(double* input, double* output, int len)
		{
			for (int i = 0; i < len; i++)
				output[i] = Process(input[i]);
		}
	};
}
