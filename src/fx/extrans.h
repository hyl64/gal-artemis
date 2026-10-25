// extrans.h - the cross-fade between the screen now and the screen being built
//
// Thirty-four uses in the corpus, and the tag carries no picture names at all:
//
//   {"bg",  file="bg07d", time=500, ...}
//   {"bg",  lv=9, id=9, file="cinema01", time=0, ...}
//   {"extrans", time=500}                     <- 34 of these, time 0..1000
//   {"extrans", time=250, sync=0}             <- some are asynchronous
//
// So "two images" are not two files named by the tag.  A is the frame that is
// on screen when `extrans` is reached; B is the frame the bg/fg/cg commands
// *before* it have already queued.  `extrans` is the moment the engine shows B
// by fading from A.  That reading is what the surrounding commands say: every
// sample has one or more bg/fg/cg tags immediately in front of the extrans and
// the next line is `{"text"}`, i.e. the picture change is complete before the
// line is read.  `sync=0` means the transition does not hold the script (the
// same meaning the bg/fg tags give it); absent `sync` means it does hold it.
//
// One real use is the bare `{"extrans"}`: no `time` means an instant switch,
// which is the same thing as time=0.  The self test pins that case.
//
// Arithmetic: every channel, including alpha, is
//
//   out = (a * (T - t) + b * t + T/2) / T      for 0 < t < T
//
// with integer division, so t=0 is exactly A, t=T exactly B, and t=T/2 is the
// rounded average - half A plus half B, not 127/128 of either.  Stage frames are
// opaque, so their alpha stays 255 naturally; mixing alpha as well keeps a
// translucent layer correct when a caller uses the same mixer on one.

#ifndef ARTEMIS_FX_EXTRANS_H
#define ARTEMIS_FX_EXTRANS_H

#include "tween.h" // AttrNameValue

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace artemis
{
namespace fx
{
	struct ExtransTag
	{
		int64_t timeMs = 0;  // the real values: 0, 1, 100, 250, 300, 500, 800, 1000
		bool hasTime = false;// false is the bare {"extrans"}: instant
		int sync = 1;        // 1 = the script waits; 0 = it runs on
		bool hasSync = false;
		bool blocksScript() const { return sync != 0; }
	};

	// `time` is optional (missing = 0), `sync` is optional (missing = 1).
	bool ParseExtransTag(const std::vector<AttrNameValue>& attrs, ExtransTag& out,
		std::string& note);

	// Q10 progress through the transition, 0..1024, for a status line.
	int64_t MixWeight(int64_t tMs, int64_t timeMs);

	// The blended value of one channel.  time<=0 or t>=time is B, t<=0 is A.
	uint8_t MixChannel(uint8_t a, uint8_t b, int64_t tMs, int64_t timeMs);

	// out = mix(A, B, t) for straight RGBA; out may not overlap a or b.
	void MixFrame(const uint8_t* a, const uint8_t* b, size_t pixelCount, int64_t tMs,
		int64_t timeMs, uint8_t* out);
}
} // namespace artemis::fx

#endif // ARTEMIS_FX_EXTRANS_H
