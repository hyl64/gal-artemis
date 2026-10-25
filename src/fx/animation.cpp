// animation.cpp - see animation.h

#include "fx/animation.h"

namespace artemis
{
namespace fx
{
	int64_t AnimeKeyHoldMs(const AnimeTrack& track, size_t index)
	{
		if(index + 1 < track.keys.size())
			return track.keys[index + 1].arriveMs;
		return track.endHoldMs;
	}

	int64_t AnimeTrackDurationMs(const AnimeTrack& track, int loop)
	{
		if(track.keys.empty())
			return 0;
		int64_t once = 0;
		for(size_t i = 0; i < track.keys.size(); i++)
			once += AnimeKeyHoldMs(track, i);
		if(loop < 0)
			return -1;
		return once * (int64_t)(1 + loop);
	}

	AnimeSample SampleAnime(const AnimeTrack& track, int64_t tMs, int loop)
	{
		AnimeSample out;
		if(track.keys.empty())
		{
			out.ended = true;
			return out;
		}
		int64_t once = 0;
		for(size_t i = 0; i < track.keys.size(); i++)
			once += AnimeKeyHoldMs(track, i);
		if(once <= 0)
		{
			out.index = (int)track.keys.size() - 1;
			out.ended = true;
			return out;
		}
		int64_t t = tMs < 0 ? 0 : tMs;
		int64_t cycle = t / once;
		if(loop >= 0 && cycle > loop)
		{
			out.ended = true;
			out.cycle = loop;
			out.index = (int)track.keys.size() - 1;
			out.msInto = AnimeKeyHoldMs(track, track.keys.size() - 1);
			return out;
		}
		int64_t within = t % once;
		out.cycle = (int)cycle;
		int64_t walked = 0;
		for(size_t i = 0; i < track.keys.size(); i++)
		{
			const int64_t hold = AnimeKeyHoldMs(track, i);
			if(within < walked + hold || i + 1 == track.keys.size())
			{
				out.index = (int)i;
				out.msInto = within - walked;
				return out;
			}
			walked += hold;
		}
		out.index = (int)track.keys.size() - 1;
		out.msInto = within - walked;
		return out;
	}

	int64_t AnimeCycleMs(const AnimeFrames& frames)
	{
		if(frames.count <= 0 || frames.frameMs <= 0)
			return 0;
		return (int64_t)frames.count * (int64_t)frames.frameMs;
	}

	FrameSample SampleFrames(const AnimeFrames& frames, int64_t tMs)
	{
		FrameSample out;
		if(frames.count <= 0)
		{
			out.ended = true;
			return out;
		}
		const int64_t cycleMs = AnimeCycleMs(frames);
		if(cycleMs <= 0)
			return out; // a descriptor with no time holds frame 0
		int64_t t = tMs < 0 ? 0 : tMs;

		// A full run is "the first pass plus `loop` repeats"; `max` caps it.
		int64_t budget = -1;
		if(frames.loop >= 0)
			budget = (int64_t)frames.loop + 1;
		if(frames.maxCycles > 0 && (budget < 0 || frames.maxCycles < budget))
			budget = frames.maxCycles;

		int64_t cycle = t / cycleMs;
		if(budget >= 0 && cycle >= budget)
		{
			out.ended = true;
			out.cycle = (int)(budget - 1);
			out.frame = frames.count - 1; // stop on the last frame, not on frame 0
			return out;
		}
		out.cycle = (int)cycle;
		out.frame = (int)((t % cycleMs) / (int64_t)frames.frameMs);
		if(out.frame >= frames.count)
			out.frame = frames.count - 1;
		return out;
	}
}
} // namespace artemis::fx
