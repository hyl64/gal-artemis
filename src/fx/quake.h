// quake.h - the screen shake behind [quake]
//
// Four in the real scene, three distinct shapes:
//
//   [quake time="50"  count="2" size="30" dir="h"]
//   [quake time="100" count="3" size="50" dir="v"]
//   [quake count="3"  time="100" size="5"  dir="v"]
//
// `time` is the whole shake in milliseconds, `size` the amplitude in pixels,
// `count` how many jolts, `dir` the axis (h = left/right, v = up/down).  The
// shakes are one-sided offsets that decay: the first jolt is the full `size`,
// each later one is smaller, and the screen is back at 0 when `time` is up.
//
// Reproducibility is the point of this file.  A shake needs random signs, but a
// *stateful* random generator makes the offsets depend on how many frames were
// drawn before - so a dropped frame, a resize, or a skip would move the shake,
// and a test could not pin it at all.  Instead the sign of jolt n is a pure
// hash of (seed, n): asking for t=37ms twice always gives the same pixel, in
// any order, on any machine.  `fxtool quake --seed 7` prints the same sequence
// every run.

#ifndef ARTEMIS_FX_QUAKE_H
#define ARTEMIS_FX_QUAKE_H

#include <cstdint>

namespace artemis
{
namespace fx
{
	struct Quake
	{
		int sizePx = 0;       // amplitude of the first jolt, in pixels
		int count = 1;        // how many jolts over the whole duration
		bool vertical = false; // dir="v"; false means dir="h"
		int64_t timeMs = 0;   // whole shake
		uint64_t seed = 0;    // same seed, same shake
	};

	struct QuakeOffset
	{
		int dx = 0;
		int dy = 0;
		int phase = -1; // which jolt; -1 once the shake is over
		bool active() const { return phase >= 0; }
	};

	// The offset at `tMs`.  Zero (phase -1) before 0 and at/after timeMs, so a
	// caller can just add dx/dy to every blit without a special case.
	QuakeOffset QuakeAt(const Quake& quake, int64_t tMs);

	// The mixed hash of (seed, jolt).  Exposed so a test can prove it is a pure
	// function of its arguments rather than a sequence.
	uint64_t QuakeHash(uint64_t seed, int phase);

	// The direction of jolt `phase`: +1 or -1, from the high bit of that hash.
	int QuakeDirection(uint64_t seed, int phase);
}
} // namespace artemis::fx

#endif // ARTEMIS_FX_QUAKE_H
