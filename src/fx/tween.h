// tween.h - time interpolation, shaped by the easings the real scripts use
//
// Two tags in the real corpus animate a number over time, and they look alike
// but are not:
//
//   [lytween id="81.0" param="top" ease="easeinout_quart"
//            from="40" to="35" time="1500" yoyo="-1"]
//       absolute: from -> to, and `yoyo` makes it run back and forth.
//
//   [scetween mode="add" type="out" param="alpha" ease="easeout_quart"
//             diff="-255" time="400" delay="0"]
//       relative: current + diff, a GUI state slot (`type`) picks the layer,
//       and `delay` holds the start (the real file passes `$g.msgspeed`).
//
// The set of easings is not open-ended: all 60 ease attributes in the corpus
// are one of `none` (36), `easeout_sine` (16), `easeout_quad` (4),
// `easeout_quart` (2), `easeinout_quart` (2).  `none` means *linear here* -
// those 36 are alpha/top ramps that plainly do travel, so "none" is the absence
// of a curve, not the absence of motion.  A few more curves are accepted
// because the naming scheme is regular, but an unknown name is *reported*, not
// quietly treated as linear: a typo in a script should be findable.
//
// Arithmetic: the interpolation itself is one double multiply per value, and
// the result is rounded half-away-from-zero and then clamped to the span
// [min(from,to), max(from,to)] - a tween that overshoots its own endpoints is
// never what a script asked for.  Everything that touches pixels is integer
// (see brightness.h); a tween produces one number per frame, so a float
// multiply there costs nothing and keeps the curves exact.

#ifndef ARTEMIS_FX_TWEEN_H
#define ARTEMIS_FX_TWEEN_H

#include <cstdint>
#include <string>
#include <vector>

namespace artemis
{
namespace fx
{
	enum class Ease
	{
		Linear,      // "none" in the scripts
		InSine,
		OutSine,
		InOutSine,
		InQuad,
		OutQuad,
		InOutQuad,
		InCubic,
		OutCubic,
		InOutCubic,
		InQuart,
		OutQuart,
		InOutQuart,
		InQuint,
		OutQuint,
		InOutQuint,
	};

	// `none`, `linear`, `easeout_quad`, `easeinout_quart`, ...  Returns false for
	// a name no curve matches; `*out` is then left alone so the caller can count
	// the miss instead of guessing.
	bool ParseEase(const std::string& name, Ease& out);

	// The canonical script spelling of a curve ("easeout_quad"), for reports.
	const char* EaseName(Ease ease);

	// The curve itself: p in [0,1] -> [0,1], with f(0)==0 and f(1)==1.
	double EaseUnit(Ease ease, double p);

	struct Tween
	{
		int64_t from = 0;
		int64_t to = 0;
		int64_t durationMs = 0;
		int64_t delayMs = 0; // real: scetween delay="$g.msgspeed" (30)
		Ease ease = Ease::Linear;
		int yoyo = 0; // 0 = play once; -1 = forever; N = N reversals after the first pass
		bool sync = true; // lytween sync="0" does not hold the script (not a value)

		// Number of passes before the value stops moving: 1 without yoyo,
		// infinite for yoyo < 0, else 1 + yoyo (so the last pass is a forward run
		// whenever the count is odd, which is what yoyo="30" reads like).
		int64_t passes() const { return yoyo < 0 ? -1 : 1 + (int64_t)yoyo; }

		// The value on a never-ending tween is periodic from here on.
		int64_t totalMs() const; // < 0 means "never stops"
	};

	// The tweened value at `tMs` after the tween started (delay included).
	// Before the delay elapses the value is `from`; after the last pass it is
	// frozen at the last pass's end value.
	int64_t ValueAt(const Tween& tween, int64_t tMs);

	// A 4-channel colour tween (R,G,B,A), same curve on every channel.
	struct ColorTween
	{
		uint8_t from[4] = { 0, 0, 0, 255 };
		uint8_t to[4] = { 0, 0, 0, 255 };
		int64_t durationMs = 0;
		int64_t delayMs = 0;
		Ease ease = Ease::Linear;
		int yoyo = 0;
	};
	void ColorAt(const ColorTween& tween, int64_t tMs, uint8_t out[4]);

	// `[scetween ...]` as a value source: the delta form of a tween.  The layer's
	// current value is `base`, so the value at t is base + diff * curve(p).
	// An `init` mode scetween has no diff/time at all and is a pure style set,
	// which the caller applies as base = value; this struct is for `add`.
	struct Scetween
	{
		int64_t diff = 0;
		int64_t durationMs = 400; // the real files always say 400
		int64_t delayMs = 0;
		Ease ease = Ease::Linear;
	};
	int64_t ScetweenValueAt(const Scetween& tween, int64_t base, int64_t tMs);

	// A tween produces a number; it does not know what the number means.  An
	// alpha ramp of -255 from 200 interpolates to -55 halfway, and -55 is the
	// honest answer from the curve - it is the *layer* that has a legal range.
	// This is that range check, in one place, so the renderer and the test agree
	// on it: alpha is 0..255, top/left are unbounded, a scale stops at 0.
	int64_t ClampProperty(int64_t value, int64_t low, int64_t high);

	// `[lytween id="81.0" param="top" ease="easeinout_quart" from="40" to="35"
	//           time="1500" yoyo="-1"]`, read the way the corpus actually writes it.
	//
	// Two of the sixteen tags in the corpus are broken:
	//   [lytween id="1" param="yscale" from="100" top="100" time="20000" sync="0"]
	//   [lytween id="1" param="xscale" from="100" top="100" time="20000" sync="0"]
	// `top=` was typed where `to=` belongs, so there is no destination.  Guessing
	// one (from `top`? from `from`?) would silently animate something the script
	// never asked for, so this parser *refuses* the tag and says why; the caller
	// counts the refusal.  Real data contains bad data, and the engine has to say
	// so rather than invent a picture.
	struct AttrNameValue
	{
		std::string name;
		std::string value;
	};

	struct LytweenTag
	{
		std::string id;
		std::string param;  // alpha, top, left, xscale, yscale, rotate
		fx::Tween tween;
		bool usable = false; // false when the tag cannot be acted on
		std::string note;    // why, in the words a report can print
	};

	bool ParseLytween(const std::vector<AttrNameValue>& attrs, LytweenTag& out);

	// A strict integer read: "$g.msgspeed" is not 0, it is *not a number*, and a
	// tag that passes a variable here is reported instead of animating from 0.
	bool ParseIntValue(const std::string& text, int64_t& out);
}
} // namespace artemis::fx

#endif // ARTEMIS_FX_TWEEN_H
