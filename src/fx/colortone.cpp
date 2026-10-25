// colortone.cpp - see colortone.h

#include "fx/colortone.h"

namespace artemis
{
namespace fx
{
	namespace
	{
		int ClampByte(int64_t value)
		{
			if(value < 0)
				return 0;
			if(value > 255)
				return 255;
			return (int)value;
		}
	}

	bool ParseColorStyle(const std::string& name, ColorStyle& out)
	{
		if(name == "reset")
		{
			out = ColorStyle::Reset;
			return true;
		}
		if(name == "sepia")
		{
			out = ColorStyle::Sepia;
			return true;
		}
		return false;
	}

	const char* ColorStyleName(ColorStyle style)
	{
		switch(style)
		{
		case ColorStyle::Reset: return "reset";
		case ColorStyle::Sepia: return "sepia";
		}
		return "?";
	}

	ColorMatrix StyleMatrix(ColorStyle style)
	{
		ColorMatrix matrix; // the default is the identity
		if(style == ColorStyle::Sepia)
		{
			// The classic sepia: R'=0.393R+0.769G+0.189B, G'=0.349R+0.686G+
			// 0.168B, B'=0.272R+0.534G+0.131B, rounded to Q10.  Written here as
			// integers so every machine grades a byte the same way.
			matrix.m[0][0] = 402;
			matrix.m[0][1] = 787;
			matrix.m[0][2] = 194;
			matrix.m[1][0] = 357;
			matrix.m[1][1] = 703;
			matrix.m[1][2] = 172;
			matrix.m[2][0] = 279;
			matrix.m[2][1] = 547;
			matrix.m[2][2] = 134;
		}
		return matrix;
	}

	bool IsIdentity(const ColorMatrix& matrix)
	{
		static const int32_t identity[3][3] = { { 1024, 0, 0 }, { 0, 1024, 0 },
			{ 0, 0, 1024 } };
		for(int c = 0; c < 3; c++)
		{
			if(matrix.bias[c] != 0)
				return false;
			for(int i = 0; i < 3; i++)
				if(matrix.m[c][i] != identity[c][i])
					return false;
		}
		return true;
	}

	uint8_t MatrixChannel(const ColorMatrix& matrix, int channel, int r, int g, int b)
	{
		if(channel < 0 || channel > 2)
			return 0;
		const int64_t sum = (int64_t)matrix.m[channel][0] * r
			+ (int64_t)matrix.m[channel][1] * g
			+ (int64_t)matrix.m[channel][2] * b
			+ matrix.bias[channel] + 512;
		return (uint8_t)ClampByte(sum >> 10);
	}

	void ApplyColorMatrix(uint8_t* rgba, size_t pixelCount, const ColorMatrix& matrix)
	{
		if(rgba == nullptr || pixelCount == 0)
			return;
		if(IsIdentity(matrix))
			return; // reset costs nothing: do not even walk the frame, and no
			        // value can drift through the round-trip
		// nine tables: table[c][i][v] is coefficient (c,i) applied to input v
		int32_t table[3][3][256];
		for(int c = 0; c < 3; c++)
			for(int i = 0; i < 3; i++)
				for(int v = 0; v < 256; v++)
					table[c][i][v] = matrix.m[c][i] * v;
		const int32_t b0 = matrix.bias[0] + 512;
		const int32_t b1 = matrix.bias[1] + 512;
		const int32_t b2 = matrix.bias[2] + 512;
		uint8_t* at = rgba;
		for(size_t p = 0; p < pixelCount; p++, at += 4)
		{
			const int r = at[0];
			const int g = at[1];
			const int b = at[2];
			at[0] = (uint8_t)ClampByte((table[0][0][r] + table[0][1][g]
				+ table[0][2][b] + b0) >> 10);
			at[1] = (uint8_t)ClampByte((table[1][0][r] + table[1][1][g]
				+ table[1][2][b] + b1) >> 10);
			at[2] = (uint8_t)ClampByte((table[2][0][r] + table[2][1][g]
				+ table[2][2][b] + b2) >> 10);
			// at[3] (alpha) stays as it is; see the header
		}
	}

	void ApplyColorTone(uint8_t* rgba, size_t pixelCount, ColorStyle style)
	{
		ApplyColorMatrix(rgba, pixelCount, StyleMatrix(style));
	}

	bool ParseColorToneTag(const std::vector<AttrNameValue>& attrs, ColorToneTag& out)
	{
		out = ColorToneTag{};
		for(const AttrNameValue& attr : attrs)
		{
			if(attr.name != "style")
				continue;
			out.style = attr.value;
			out.valid = ParseColorStyle(attr.value, out.value);
			if(!out.valid)
				out.note = "unknown colour style \"" + attr.value + "\"";
			return out.valid;
		}
		out.note = "colortone tag without a style attribute";
		return false;
	}
}
} // namespace artemis::fx
