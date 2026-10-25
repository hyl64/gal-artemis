// brightness.h - the screen brightness ramp behind [sfade]
//
// The real scene has 14 of these, all of one shape:
//
//   [sfade time="2000" gain="500"]    the tanabata scene, the one that shows
//   [sfade time="500"  gain="1000"]
//   [sfade time="500"  gain="0"]      ... and gain=2000 style brightenings are
//                                     *not* in this corpus
//
// `gain` is thousandths: 1000 is the picture as it is, 500 is half brightness,
// 0 is black, and a ramp of `time` ms takes the screen from whatever it was to
// that value.  So `sfade` is a brightness *target plus duration*, not a delta -
// the opposite of scetween, which is a delta with no target.
//
// The arithmetic is integer on purpose.  Every channel does
//
//     out = clamp((in * gain + 500) / 1000, 0, 255)
//
// with integer division, i.e. rounded to nearest, and `gain` itself clamped to
// [0, 4000] so a broken argument cannot overflow.  gain = 1000 is *exactly* the
// identity for all 256 values (the +500 rounds the remainder up to nothing),
// which the self test checks value by value.  gain = 500 gives (in+1)/2: 255 ->
// 128, not 127, because the +500 rounds half up.
//
// Alpha is left alone.  A screen fade dims the composite, and the composite is
// opaque; scaling alpha as well would double-dim a translucent layer and would
// break the canvas the renderer hands back.  Layer transparency is `lyprop
// alpha`, which is a different tag with a different meaning.

#ifndef ARTEMIS_FX_BRIGHTNESS_H
#define ARTEMIS_FX_BRIGHTNESS_H

#include <cstddef>
#include <cstdint>

namespace artemis
{
namespace fx
{
	// The gain a full-brightness screen has.
	constexpr int kBrightnessIdentity = 1000;
	// How far a gain may be pushed before it is clamped: 4x, well past anything
	// a script writes, and small enough that in*gain cannot overflow an int.
	constexpr int kBrightnessMaxGain = 4000;

	int ClampGain(int gain);

	// One channel through the formula above.
	uint8_t ScaleChannel(uint8_t value, int gain);

	// All 256 channel values, so a whole frame costs one lookup per channel.
	void BuildGainLut(uint8_t lut[256], int gain);

	// Apply the gain in place to straight RGBA (as src/pngio.h returns it):
	// three lookups per pixel, alpha copied.  `pixelCount` is w*h.
	void ApplyGain(uint8_t* rgba, size_t pixelCount, int gain);

	// The gain `tMs` into a fade from `fromGain` to `toGain` over `timeMs`.
	// Integer, rounded half up, clamped to the span; `timeMs` <= 0 jumps to the
	// target, and after the end it stays there.
	int GainAt(int64_t tMs, int fromGain, int toGain, int64_t timeMs);
}
} // namespace artemis::fx

#endif // ARTEMIS_FX_BRIGHTNESS_H
