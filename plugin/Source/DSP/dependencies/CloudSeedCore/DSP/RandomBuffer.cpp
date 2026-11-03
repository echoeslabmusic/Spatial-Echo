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

#include <climits>
#include "RandomBuffer.h"
#include "LcgRandom.h"

namespace Cloudseed
{
	void RandomBuffer::Fill(uint64_t seed, int count, float* output)
	{
		if (output == nullptr || count <= 0)
			return;

		LcgRandom rand(seed);
		for (int i = 0; i < count; i++)
		{
			unsigned int val = rand.NextUInt();
			output[i] = val / (float)UINT_MAX;
		}
	}

	void RandomBuffer::Fill(uint64_t seed, int count, float crossSeed, float* output)
	{
		if (output == nullptr || count <= 0)
			return;

		LcgRandom seriesA(seed);
		LcgRandom seriesB(~seed);
		for (int i = 0; i < count; i++)
		{
			const float valA = seriesA.NextUInt() / (float)UINT_MAX;
			const float valB = seriesB.NextUInt() / (float)UINT_MAX;
			output[i] = valA * (1 - crossSeed) + valB * crossSeed;
		}
	}

	std::vector<float> RandomBuffer::Generate(uint64_t seed, int count)
	{
		if (count <= 0)
			return {};

		std::vector<float> output(static_cast<size_t>(count), 0.0f);
		Fill(seed, count, output.data());
		return output;
	}

	std::vector<float> RandomBuffer::Generate(uint64_t seed, int count, float crossSeed)
	{
		if (count <= 0)
			return {};

		std::vector<float> output(static_cast<size_t>(count), 0.0f);
		Fill(seed, count, crossSeed, output.data());

		return output;
	}
}
