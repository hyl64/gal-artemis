// colortone.h - colour grading for a whole frame or one layer
//
// The corpus writes colour tone as a *name*, not as a matrix:
//
//   {"colortone", style="reset"}    132 times  (the grade is taken back off)
//   {"colortone", style="sepia"}      9 times  (a memory / flashback grade)
//   {"fg", ..., style="sepia", ...} 146 times  (the same grade on one sprite)
//
// There is no `r=`, `g=`, `b=`, `matrix=` or 4x5 coefficient list anywhere in
// the 59 scripts.  So the engine-facing unit is a style name, and the matrix is
// an implementation detail behind it - but it *is* a matrix, because that is
// the only shape that can express both `sepia` and `reset` and still take a
// per-layer and a whole-frame path through the same arithmetic.
//
// The matrix is Q10 fixed point, one 3x3 block plus a per-channel bias:
//
//   out[c] = clamp((sum_i m[c][i] * in[i] + bias[c] + 512) >> 10, 0, 255)
//
// `+512` is round-half-up before the shift; `clamp` is 0..255.  Alpha is never
// touched: a colour grade changes how a pixel looks, not whether it is there,
// and scaling alpha would make a translucent sprite double-dim.
//
// `reset` is the exact identity matrix, and the self test walks all 256 values
// through it to prove no rounding drift.  `sepia` is the classic 3x3 matrix
// (0.393/0.769/0.189 ...); with the corpus carrying no reference pixels, that
// is a documented choice, not a recovered constant.  White grades to
// (255,255,239), black stays black, pure red to (100,89,69).
//
// Performance: applying a matrix is nine 256-entry lookup tables (built once
// per grade) and three adds per output channel, so a 1024x698 stage frame costs
// a few milliseconds; the self test prints the measured number.

#ifndef ARTEMIS_FX_COLORTONE_H
#define ARTEMIS_FX_COLORTONE_H

#include "tween.h" // AttrNameValue, the shared {...} attribute list

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace artemis
{
namespace fx
{
	enum class ColorStyle
	{
		Reset,
		Sepia,
	};

	// "reset" / "sepia"; false and `out` untouched for a name this tree does not
	// know, so a report can count it instead of silently painting something.
	bool ParseColorStyle(const std::string& name, ColorStyle& out);
	const char* ColorStyleName(ColorStyle style);

	// Q10 fixed point: 1024 is 1.0.
	struct ColorMatrix
	{
		int32_t m[3][3] = { { 1024, 0, 0 }, { 0, 1024, 0 }, { 0, 0, 1024 } };
		int32_t bias[3] = { 0, 0, 0 };
	};

	ColorMatrix StyleMatrix(ColorStyle style);
	bool IsIdentity(const ColorMatrix& matrix);

	// One channel of the formula above; exposed so the self test can pin exact
	// bytes rather than only whole images.
	uint8_t MatrixChannel(const ColorMatrix& matrix, int channel, int r, int g, int b);

	// In-place on straight RGBA (src/pngio.h layout), `pixelCount` = w*h.
	// Alpha is copied unchanged; the identity matrix returns immediately.
	void ApplyColorMatrix(uint8_t* rgba, size_t pixelCount, const ColorMatrix& matrix);
	void ApplyColorTone(uint8_t* rgba, size_t pixelCount, ColorStyle style);

	// `{"colortone", style="sepia"}` and `{"fg", style="sepia"}` parse the same
	// way: the tag is the style name.
	struct ColorToneTag
	{
		std::string style;
		ColorStyle value = ColorStyle::Reset;
		bool valid = false;
		std::string note;
	};
	bool ParseColorToneTag(const std::vector<AttrNameValue>& attrs, ColorToneTag& out);
}
} // namespace artemis::fx

#endif // ARTEMIS_FX_COLORTONE_H
