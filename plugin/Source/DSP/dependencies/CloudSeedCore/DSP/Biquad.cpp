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

// [vendored] _USE_MATH_DEFINES moved before any includes for MSVC compat
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <cmath>
#include "Biquad.h"

namespace Cloudseed
{
	Biquad::Biquad()
	{
		ClearBuffers();
	}

	Biquad::Biquad(FilterType filterType, double fs)
	{
		Type = filterType;
		SetSamplerate(fs);

		SetGainDb(0.0);
		Frequency = fs * 0.25;
		SetQ(0.5);
		ClearBuffers();
	}

	Biquad::~Biquad()
	{

	}


	double Biquad::GetSamplerate()
	{
		return fs;
	}

	void Biquad::SetSamplerate(double fs)
	{
		this->fs = fs;
		fsInv = 1.0 / fs;
		Update();
	}

	double Biquad::GetGainDb()
	{
		return gainDB;
	}

	double Biquad::GetGain()
	{
		return gain;
	}

	void Biquad::SetGainDb (double value)
	{
			// Clamp value between -60 and 60
			if (value < -60.0)
				value = -60.0;
			else if (value > 60.0)
				value = 60.0;

			gainDB = value;
			gain = std::pow(10.0, value / 20.0);
	}

	void Biquad::SetGain (double value)
	{
		if (value < 0.001)
			value = 0.001; // -60dB
		else if (value > 1000.0)
			value = 1000.0; // 60dB

		gain = value;
		gainDB = std::log10(gain) * 20.0;
	}

	double Biquad::GetQ()
	{
		return q;
	}

	void Biquad::SetQ(double value)
	{
		if (value < 0.001)
			value = 0.001;
		q = value;
	}

	// this is the newer set of formulas from http://www.earlevel.com/main/2011/01/02/biquad-formulas/
	// Note that for shelf and peak filters, I had to invert the if/else statements for boost and cut, as
	// I was getting the inverse desired effect, very odd...
	void Biquad::Update()
	{
		auto Fc = Frequency;

		auto V = std::pow(10.0, std::fabs(gainDB) / 20.0);
		auto K = std::tan(M_PI * Fc * fsInv);
		auto Q = q;
		double norm = 1.0;

		switch (Type)
		{
		case FilterType::LowPass6db:
			a1 = -std::exp(-2.0 * M_PI * (Fc * fsInv));
			b0 = 1.0 + a1;
			b1 = b2 = a2 = 0.0;
			break;
		case FilterType::HighPass6db:
			a1 = -std::exp(-2.0 * M_PI * (Fc * fsInv));
			b0 = a1;
			b1 = -a1;
			b2 = a2 = 0.0;
			break;
		case FilterType::LowPass:
			norm = 1.0 / (1.0 + K / Q + K * K);
			b0 = K * K * norm;
			b1 = 2.0 * b0;
			b2 = b0;
			a1 = 2.0 * (K * K - 1.0) * norm;
			a2 = (1.0 - K / Q + K * K) * norm;
			break;
		case FilterType::HighPass:
			norm = 1.0 / (1.0 + K / Q + K * K);
			b0 = 1.0 * norm;
			b1 = -2.0 * b0;
			b2 = b0;
			a1 = 2.0 * (K * K - 1.0) * norm;
			a2 = (1.0 - K / Q + K * K) * norm;
			break;
		case FilterType::BandPass:
			norm = 1.0 / (1.0 + K / Q + K * K);
			b0 = K / Q * norm;
			b1 = 0.0;
			b2 = -b0;
			a1 = 2.0 * (K * K - 1.0) * norm;
			a2 = (1.0 - K / Q + K * K) * norm;
			break;
		case FilterType::Notch:
			norm = 1.0 / (1.0 + K / Q + K * K);
			b0 = (1.0 + K * K) * norm;
			b1 = 2.0 * (K * K - 1.0) * norm;
			b2 = b0;
			a1 = b1;
			a2 = (1.0 - K / Q + K * K) * norm;
			break;
		case FilterType::Peak:
			if (gainDB >= 0.0)
			{
				norm = 1.0 / (1.0 + 1.0 / Q * K + K * K);
				b0 = (1.0 + V / Q * K + K * K) * norm;
				b1 = 2.0 * (K * K - 1.0) * norm;
				b2 = (1.0 - V / Q * K + K * K) * norm;
				a1 = b1;
				a2 = (1.0 - 1.0 / Q * K + K * K) * norm;
			}
			else
			{
				norm = 1.0 / (1.0 + V / Q * K + K * K);
				b0 = (1.0 + 1.0 / Q * K + K * K) * norm;
				b1 = 2.0 * (K * K - 1.0) * norm;
				b2 = (1.0 - 1.0 / Q * K + K * K) * norm;
				a1 = b1;
				a2 = (1.0 - V / Q * K + K * K) * norm;
			}
			break;
		case FilterType::LowShelf:
			if (gainDB >= 0.0)
			{
				norm = 1.0 / (1.0 + std::sqrt(2.0) * K + K * K);
				b0 = (1.0 + std::sqrt(2.0 * V) * K + V * K * K) * norm;
				b1 = 2.0 * (V * K * K - 1.0) * norm;
				b2 = (1.0 - std::sqrt(2.0 * V) * K + V * K * K) * norm;
				a1 = 2.0 * (K * K - 1.0) * norm;
				a2 = (1.0 - std::sqrt(2.0) * K + K * K) * norm;
			}
			else
			{
				norm = 1.0 / (1.0 + std::sqrt(2.0 * V) * K + V * K * K);
				b0 = (1.0 + std::sqrt(2.0) * K + K * K) * norm;
				b1 = 2.0 * (K * K - 1.0) * norm;
				b2 = (1.0 - std::sqrt(2.0) * K + K * K) * norm;
				a1 = 2.0 * (V * K * K - 1.0) * norm;
				a2 = (1.0 - std::sqrt(2.0 * V) * K + V * K * K) * norm;
			}
			break;
		case FilterType::HighShelf:
			if (gainDB >= 0.0)
			{
				norm = 1.0 / (1.0 + std::sqrt(2.0) * K + K * K);
				b0 = (V + std::sqrt(2.0 * V) * K + K * K) * norm;
				b1 = 2.0 * (K * K - V) * norm;
				b2 = (V - std::sqrt(2.0 * V) * K + K * K) * norm;
				a1 = 2.0 * (K * K - 1.0) * norm;
				a2 = (1.0 - std::sqrt(2.0) * K + K * K) * norm;
			}
			else
			{
				norm = 1.0 / (V + std::sqrt(2.0 * V) * K + K * K);
				b0 = (1.0 + std::sqrt(2.0) * K + K * K) * norm;
				b1 = 2.0 * (K * K - 1.0) * norm;
				b2 = (1.0 - std::sqrt(2.0) * K + K * K) * norm;
				a1 = 2.0 * (K * K - V) * norm;
				a2 = (V - std::sqrt(2.0 * V) * K + K * K) * norm;
			}
			break;
		}
	}

	double Biquad::GetResponse(float freq) const
	{
		double phi = pow((sin(2 * M_PI * freq / (2.0 * fs))), 2);
		double y = ((pow(b0 + b1 + b2, 2.0) - 4.0 * (b0 * b1 + 4.0 * b0 * b2 + b1 * b2) * phi + 16.0 * b0 * b2 * phi * phi) / (pow(1.0 + a1 + a2, 2.0) - 4.0 * (a1 + 4.0 * a2 + a1 * a2) * phi + 16.0 * a2 * phi * phi));
		// y gives you power gain, not voltage gain, and this a 10 * log_10(g) formula instead of 20 * log_10(g)
		// by taking the sqrt we get a value that's more suitable for signal processing, i.e. the voltage gain
		return sqrt(y);
	}

	void Biquad::ClearBuffers()
	{
		y = 0;
		x2 = 0;
		y2 = 0;
		x1 = 0;
		y1 = 0;
	}
}
