// extrans.cpp - see extrans.h

#include "fx/extrans.h"

#include <cstring>

namespace artemis
{
namespace fx
{
	namespace
	{
		int ClampInt(int64_t value, int low, int high)
		{
			if(value < low)
				return low;
			if(value > high)
				return high;
			return (int)value;
		}
	}

	bool ParseExtransTag(const std::vector<AttrNameValue>& attrs, ExtransTag& out,
		std::string& note)
	{
		out = ExtransTag{};
		for(const AttrNameValue& attr : attrs)
		{
			if(attr.name == "time")
			{
				int64_t value = 0;
				if(!ParseIntValue(attr.value, value))
				{
					note = "extrans time \"" + attr.value + "\" is not a number";
					return false;
				}
				out.timeMs = value;
				out.hasTime = true;
			}
			else if(attr.name == "sync")
			{
				int64_t value = 0;
				if(!ParseIntValue(attr.value, value))
				{
					note = "extrans sync \"" + attr.value + "\" is not a number";
					return false;
				}
				out.sync = ClampInt(value, -1, 1);
				out.hasSync = true;
			}
		}
		if(!out.hasTime)
			note = "no time: the switch is instant";
		return true;
	}

	int64_t MixWeight(int64_t tMs, int64_t timeMs)
	{
		if(timeMs <= 0 || tMs >= timeMs)
			return 1024;
		if(tMs <= 0)
			return 0;
		return (tMs * 1024 + timeMs / 2) / timeMs;
	}

	uint8_t MixChannel(uint8_t a, uint8_t b, int64_t tMs, int64_t timeMs)
	{
		if(timeMs <= 0 || tMs >= timeMs)
			return b;
		if(tMs <= 0)
			return a;
		const int64_t sum = (int64_t)a * (timeMs - tMs) + (int64_t)b * tMs + timeMs / 2;
		return (uint8_t)ClampInt(sum / timeMs, 0, 255);
	}

	void MixFrame(const uint8_t* a, const uint8_t* b, size_t pixelCount, int64_t tMs,
		int64_t timeMs, uint8_t* out)
	{
		if(a == nullptr || b == nullptr || out == nullptr || pixelCount == 0)
			return;
		if(timeMs <= 0 || tMs >= timeMs)
		{
			memcpy(out, b, pixelCount * 4);
			return;
		}
		if(tMs <= 0)
		{
			memcpy(out, a, pixelCount * 4);
			return;
		}
		for(size_t p = 0; p < pixelCount; p++)
			for(int c = 0; c < 4; c++)
				out[p * 4 + c] = MixChannel(a[p * 4 + c], b[p * 4 + c], tMs, timeMs);
	}
}
} // namespace artemis::fx
