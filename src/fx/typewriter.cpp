// typewriter.cpp - see typewriter.h

#include "fx/typewriter.h"

namespace artemis
{
namespace fx
{
	namespace
	{
		// Decode one code point.  Returns false for a malformed sequence and
		// leaves `length` at 1.
		bool DecodeOne(const std::string& s, size_t at, uint32_t& cp, uint32_t& length)
		{
			const uint8_t b0 = (uint8_t)s[at];
			if(b0 < 0x80)
			{
				cp = b0;
				length = 1;
				return true;
			}
			int need = 0;
			uint32_t value = 0;
			if((b0 & 0xE0) == 0xC0)
			{
				need = 1;
				value = b0 & 0x1F;
			}
			else if((b0 & 0xF0) == 0xE0)
			{
				need = 2;
				value = b0 & 0x0F;
			}
			else if((b0 & 0xF8) == 0xF0)
			{
				need = 3;
				value = b0 & 0x07;
			}
			else
			{
				cp = 0xFFFD;
				length = 1;
				return false;
			}
			if(at + (size_t)need >= s.size())
			{
				cp = 0xFFFD;
				length = 1;
				return false;
			}
			for(int i = 1; i <= need; i++)
			{
				const uint8_t b = (uint8_t)s[at + (size_t)i];
				if((b & 0xC0) != 0x80)
				{
					cp = 0xFFFD;
					length = 1;
					return false;
				}
				value = (value << 6) | (b & 0x3F);
			}
			// over-long forms and surrogates are not text
			static const uint32_t kMin[4] = { 0, 0x80, 0x800, 0x10000 };
			if(value < kMin[need] || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
			{
				cp = 0xFFFD;
				length = 1;
				return false;
			}
			cp = value;
			length = (uint32_t)need + 1;
			return true;
		}

		// Marks that belong to the glyph in front of them: the combining block,
		// the two Japanese voicing marks, variation selectors and the joiner.
		bool IsCombining(uint32_t cp)
		{
			if(cp >= 0x0300 && cp <= 0x036F)
				return true; // combining diacritical marks
			if(cp >= 0x1AB0 && cp <= 0x1AFF)
				return true; // combining diacritical marks extended
			if(cp >= 0x3099 && cp <= 0x309A)
				return true; // combining (han)dakuten
			if(cp >= 0xFE00 && cp <= 0xFE0F)
				return true; // variation selectors
			if(cp == 0x200D)
				return true; // zero width joiner
			return false;
		}

		bool IsBreak(uint32_t cp) { return cp == '\n' || cp == '\r'; }
	}

	Text Segment(const std::string& utf8)
	{
		Text out;
		out.utf8 = utf8;
		out.glyphs.reserve(utf8.size());
		out.stops.reserve(utf8.size());
		size_t at = 0;
		while(at < utf8.size())
		{
			uint32_t cp = 0;
			uint32_t length = 1;
			const bool ok = DecodeOne(utf8, at, cp, length);
			if(!ok)
				out.badBytes++;
			if(!out.glyphs.empty() && IsCombining(cp))
			{
				// fold the mark into the glyph before it: the reader must not see a
				// lone dakuten, and it costs no extra time
				Glyph& back = out.glyphs.back();
				back.byteLength += length;
				back.codePoints++;
				out.stops.back() += length;
				at += length;
				continue;
			}
			Glyph glyph;
			glyph.codepoint = cp;
			glyph.byteOffset = (uint32_t)at;
			glyph.byteLength = length;
			glyph.codePoints = 1;
			glyph.timed = !IsBreak(cp);
			out.glyphs.push_back(glyph);
			out.stops.push_back(at + length);
			if(glyph.timed)
				out.timedGlyphs++;
			at += length;
		}
		return out;
	}

	Reveal RevealAt(const Text& text, int64_t tMs, int msPerGlyph)
	{
		Reveal out;
		const size_t total = text.timedGlyphs;
		if(total == 0)
		{
			out.complete = true;
			out.shown = text.utf8;
			return out;
		}
		if(msPerGlyph <= 0)
		{
			out.glyphs = total;
			out.bytes = text.utf8.size();
			out.complete = true;
			out.shown = text.utf8;
			return out;
		}
		const int64_t steps = tMs <= 0 ? 0 : tMs / (int64_t)msPerGlyph;
		size_t visible = (size_t)steps + 1; // the first glyph is there at t=0
		if(visible >= total)
		{
			out.glyphs = total;
			out.bytes = text.utf8.size();
			out.complete = true;
			out.shown = text.utf8;
			return out;
		}
		// Walk until `visible` timed glyphs have been placed.  Untimed glyphs on
		// the way (the page's line breaks) are taken for free, so a break that has
		// been reached is already in effect - the loop can only stop in front of a
		// *timed* glyph, which is what keeps the cut on a glyph boundary.
		size_t seen = 0;
		size_t cut = 0;
		for(size_t i = 0; i < text.glyphs.size(); i++)
		{
			if(text.glyphs[i].timed)
			{
				if(seen == visible)
					break;
				seen++;
			}
			cut = text.stops[i];
		}
		out.glyphs = visible;
		out.bytes = cut;
		out.complete = false;
		out.shown = text.utf8.substr(0, cut);
		return out;
	}

	int64_t RevealDurationMs(const Text& text, int msPerGlyph)
	{
		if(text.timedGlyphs == 0 || msPerGlyph <= 0)
			return 0;
		return (int64_t)(text.timedGlyphs - 1) * (int64_t)msPerGlyph;
	}
}
} // namespace artemis::fx
