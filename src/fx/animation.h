// animation.h - two things the corpus calls "anime", and neither is a tween
//
// 1. The script tag.  first.iet says:
//
//      [anime id="82" mode="init" file="forsystem/glyph" top="30" left="0" alpha="250"]
//      [anime id="82" mode="add"  file="forsystem/glyph" top="35" left="5" alpha="50" time="400"]
//      ...
//      [anime id="82" mode="end"  time="800"]
//
//    That is a *keyframe track*, not interpolation: each `add` arrives after its
//    `time` and then simply sits there, which is exactly how the sparkle and the
//    character blink get their flicker.  The value that moves smoothly between
//    two numbers is `[lytween]`; if this file interpolated, lytween would be
//    redundant.  The 18 occurrences in first.iet are three tracks: two glyph
//    layers (id 81.1 / 82) and one background slide (`bgimage/run` -> `run2`).
//
// 2. The IPT descriptor.  sample/ipt/image_hd_anime_line1.ipt says:
//
//      base = { x=0, y=0, w=1280, h=720, ax=640, ay=360,
//               time=100, max=3, loop=-1 }
//      {"line11"}, {"line12"}, {"line13"}
//
//    One *file* holds three frames and cycles through them every 100ms.  `loop`
//    is how many extra cycles follow the first, so -1 (the real value) means
//    forever; `max` caps the total at 3 cycles, which at 3 frames x 100ms is
//    900ms.  Read the other way round - max as "3 frames" - `max` and `loop=-1`
//    would contradict each other, so the cap reading is the only self-consistent
//    one; it is also the only thing that makes `max` mean anything here.
//    Both numbers are printed raw by `fxtool anime` so the reading can be
//    checked against a title whose animation visibly stops.

#ifndef ARTEMIS_FX_ANIMATION_H
#define ARTEMIS_FX_ANIMATION_H

#include <cstdint>
#include <string>
#include <vector>

namespace artemis
{
namespace fx
{
	// --- 1. the script tag: [anime id=... mode=init|add|end] -------------------
	struct AnimeKey
	{
		std::string file;   // empty when the tag only moves the layer
		int top = 0;
		int left = 0;
		int alpha = 255;
		bool hasFile = false;
		bool hasTop = false;
		bool hasLeft = false;
		bool hasAlpha = false;
		int64_t arriveMs = 0; // the tag's `time`: how long until this key shows
	};

	struct AnimeTrack
	{
		std::vector<AnimeKey> keys; // keys[0] is mode="init"
		int64_t endHoldMs = 0;      // mode="end" time: hold the last key, then stop
	};

	// How long key `index` stays on screen.  A key is replaced when the *next*
	// one arrives, so the last key lasts until the end tag's time runs out - that
	// is what makes the init key visible at all (it has no time of its own).
	int64_t AnimeKeyHoldMs(const AnimeTrack& track, size_t index);
	int64_t AnimeTrackDurationMs(const AnimeTrack& track, int loop); // -1 = forever

	struct AnimeSample
	{
		int index = -1;    // key on screen, -1 once the track has stopped
		int64_t msInto = 0;
		int cycle = 0;
		bool ended = false;
	};

	AnimeSample SampleAnime(const AnimeTrack& track, int64_t tMs, int loop = 0);

	// --- 2. the IPT descriptor: mode="anime_full" ------------------------------
	struct AnimeFrames
	{
		int count = 0;     // how many frames the descriptor lists
		int frameMs = 0;   // base.time
		int maxCycles = 0; // base.max; <= 0 means no cap
		int loop = 0;      // base.loop; -1 means forever
	};

	int64_t AnimeCycleMs(const AnimeFrames& frames);

	struct FrameSample
	{
		int frame = 0;   // index into the descriptor's frame list
		int cycle = 0;   // 0-based pass through the list
		bool ended = false;
	};

	// Which frame of the descriptor is on screen at `tMs`.  Stops on the last
	// frame once the loop budget (loop, capped by max) runs out.
	FrameSample SampleFrames(const AnimeFrames& frames, int64_t tMs);
}
} // namespace artemis::fx

#endif // ARTEMIS_FX_ANIMATION_H
