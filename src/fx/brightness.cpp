// brightness.cpp - see brightness.h

#include "fx/brightness.h"

namespace artemis
{
namespace fx
{
	namespace
	{
		int ClampByte(int64_t v)
		{
			if(v < 0)
				return 0;
			if(v > 255)
				return 255;
			return (int)v;
		}
	}

	int ClampGain(int gain)
	{
		if(gain < 0)
			return 0;
		if(gain > kBrightnessMaxGain)
			return kBrightnessMaxGain;
		return gain;
	}

	uint8_t ScaleChannel(uint8_t value, int gain)
	{
		const int clamped = ClampGain(gain);
		// +500 before the integer divide is round-half-up; gain = 1000 therefore
		// returns `value` exactly, which the self test walks 0..255 to prove.
		return (uint8_t)ClampByte(((int64_t)value * (int64_t)clamped + 500) / 1000);
	}

	void BuildGainLut(uint8_t lut[256], int gain)
	{
		for(int v = 0; v < 256; v++)
			lut[v] = ScaleChannel((uint8_t)v, gain);
	}

	void ApplyGain(uint8_t* rgba, size_t pixelCount, int gain)
	{
		if(rgba == nullptr || pixelCount == 0)
			return;
		const int clamped = ClampGain(gain);
		if(clamped == kBrightnessIdentity)
			return; // gain 1000 is the identity: do not even walk the frame
		uint8_t lut[256];
		BuildGainLut(lut, clamped);
		uint8_t* at = rgba;
		for(size_t i = 0; i < pixelCount; i++)
		{
			at[0] = lut[at[0]];
			at[1] = lut[at[1]];
			at[2] = lut[at[2]];
			// at[3] (alpha) stays as it is - see the header for why
			at += 4;
		}
	}

	int GainAt(int64_t tMs, int fromGain, int toGain, int64_t timeMs)
	{
		const int from = ClampGain(fromGain);
		const int to = ClampGain(toGain);
		if(timeMs <= 0 || tMs >= timeMs)
			return to;
		if(tMs <= 0)
			return from;
		const int64_t span = (int64_t)to - (int64_t)from;
		// round half away from zero, so a 1000 -> 500 ramp passes 750 at the
		// halfway mark instead of 749
		const int64_t delta = (span * tMs * 2 + (span >= 0 ? timeMs : -timeMs)) / (timeMs * 2);
		const int64_t value = (int64_t)from + delta;
		const int low = span < 0 ? to : from;
		const int high = span < 0 ? from : to;
		if(value < low)
			return low;
		if(value > high)
			return high;
		return (int)value;
	}
}
} // namespace artemis::fx
