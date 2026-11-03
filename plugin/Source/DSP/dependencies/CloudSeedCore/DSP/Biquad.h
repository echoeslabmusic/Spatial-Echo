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

namespace Cloudseed
{
	class Biquad
	{
	public:
		enum class FilterType
		{
			LowPass6db = 0,
			HighPass6db,
			LowPass,
			HighPass,
			BandPass,
			Notch,
			Peak,
			LowShelf,
			HighShelf
		};

	private:
		double fs;
		double fsInv;
		double gainDB;
		double q;
		double a0, a1, a2, b0, b1, b2;
		double x1, x2, y, y1, y2;
		double gain;

	public:
		FilterType Type;
		double Output;
		double Frequency;

		Biquad();
		Biquad(FilterType filterType, double fs);
		~Biquad();

		double GetSamplerate();
		void SetSamplerate(double fs);
		double GetGainDb();
		void SetGainDb(double value);
		double GetGain();
		void SetGain(double value);
		double GetQ();
		void SetQ(double value);

		void Update();

		double GetResponse(float freq) const;

		double inline Process(double x)
		{
			y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
			x2 = x1;
			y2 = y1;
			x1 = x;
			y1 = y;

			Output = y;
			return Output;
		}

		void inline Process(double* input, double* output, int len)
		{
			for (int i = 0; i < len; i++)
			{
				double x = input[i];
				y = ((b0 * x) + (b1 * x1) + (b2 * x2)) - (a1 * y1) - (a2 * y2);
				x2 = x1;
				y2 = y1;
				x1 = x;
				y1 = y;

				output[i] = y;
			}

			Output = y;
		}

		void ClearBuffers();
	};
}
