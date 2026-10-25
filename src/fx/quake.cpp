// quake.cpp - see quake.h

#include "fx/quake.h"

namespace artemis
{
namespace fx
{
	namespace
	{
		// splitmix64's finaliser: two multiplies and two shifts, no state.
		uint64_t Mix(uint64_t x)
		{
			x += 0x9E3779B97F4A7C15ull;
			x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
			x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
			return x ^ (x >> 31);
		}
	}

	uint64_t QuakeHash(uint64_t seed, int phase)
	{
		// the phase is folded in through a different golden-ratio constant so that
		// (seed=1, phase=2) and (seed=2, phase=1) do not collide
		return Mix(Mix(seed) ^ (0xD1B54A32D192ED03ull * (uint64_t)(phase + 1)));
	}

	int QuakeDirection(uint64_t seed, int phase)
	{
		return (QuakeHash(seed, phase) >> 63) != 0 ? 1 : -1;
	}

	QuakeOffset QuakeAt(const Quake& quake, int64_t tMs)
	{
		QuakeOffset out;
		if(quake.sizePx <= 0 || quake.count <= 0 || quake.timeMs <= 0)
			return out;
		if(tMs < 0 || tMs >= quake.timeMs)
			return out; // back at rest: the caller adds 0 without a special case

		// how long one jolt lasts; count is clamped so every jolt gets at least
		// one millisecond rather than dividing by zero on a silly argument
		int count = quake.count;
		if((int64_t)count > quake.timeMs)
			count = (int)quake.timeMs;
		const int64_t phaseMs = quake.timeMs / count;
		int phase = (int)(tMs / phaseMs);
		if(phase >= count)
			phase = count - 1;

		// the first jolt is the full size, the last one is size/count, and the
		// screen lands back on 0 exactly when timeMs is up
		const int amplitude = (int)((int64_t)quake.sizePx * (int64_t)(count - phase) / count);
		const int offset = QuakeDirection(quake.seed, phase) * amplitude;
		out.phase = phase;
		if(quake.vertical)
			out.dy = offset;
		else
			out.dx = offset;
		return out;
	}
}
} // namespace artemis::fx
