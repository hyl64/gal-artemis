// typewriter.h - revealing a page one glyph at a time
//
// The reveal is not a tag.  The real corpus never says "type this out": the
// message layer advances by itself at `$g.msgspeed`, which first.iet sets to 30
// (`[var name="g.msgspeed" data="30"]`), and the GUI feeds that same 30ms into
// `[scetween ... delay="$g.msgspeed"]` when it animates the layer in behind the
// text.  So 30 milliseconds per character is the title's own number, and it is
// the one used here.
//
// What makes this more than a substring is UTF-8.  The scripts are Shift-JIS on
// disk and the parser hands over UTF-8, where `漢` is three bytes and the small
// kana of a name can be four.  Cutting the string at "t/30 bytes" would slice a
// character in half and the renderer would draw a replacement box; every offset
// this file returns is a *glyph boundary*, and the self test pins that a cut
// inside 漢 (E6 BC A2) is unreachable.
//
// A glyph, here, is a base code point plus whatever combining marks or
// variation selectors follow it: `か` + U+3099 is one glyph the way a font sees
// it, and the reader must not see a bare dakuten float alone for 30ms.  A
// newline (`@` in the script) is a layout event, not a character, so it costs
// no time - but it is included as soon as the glyph before it is revealed.

#ifndef ARTEMIS_FX_TYPEWRITER_H
#define ARTEMIS_FX_TYPEWRITER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace artemis
{
namespace fx
{
	struct Glyph
	{
		uint32_t codepoint = 0;  // the base code point (U+FFFD for a broken byte)
		uint32_t byteOffset = 0; // where it starts in the UTF-8 text
		uint32_t byteLength = 0; // base + attached marks; 1..4 per code point
		uint8_t codePoints = 1;  // >1 when combining marks were folded in
		bool timed = true;       // false for '\n' and '\r': no ms spent
	};

	struct Text
	{
		std::string utf8;
		std::vector<Glyph> glyphs;
		// End byte offset of each glyph: the only offsets a reveal may stop on.
		std::vector<size_t> stops;
		size_t timedGlyphs = 0;
		size_t badBytes = 0; // bytes that were not valid UTF-8 (drawn as U+FFFD)
	};

	// Decode `utf8` into glyphs.  Never throws and never loses a byte: a
	// malformed sequence becomes one U+FFFD glyph covering exactly one byte, so
	// the stops stay inside the input.
	Text Segment(const std::string& utf8);

	struct Reveal
	{
		size_t glyphs = 0;  // how many timed glyphs are on screen
		size_t bytes = 0;   // prefix length of Text::utf8 to draw
		bool complete = false;
		std::string shown;  // the prefix itself, always on a glyph boundary
	};

	// The state of the reveal `tMs` after the page appeared, at `msPerGlyph`
	// (30 in the real title).  The first glyph is on screen at t=0, so a page of
	// n glyphs is complete at (n-1)*msPerGlyph.  msPerGlyph <= 0 reveals
	// everything at once, which is what the "instant" text-speed setting means.
	Reveal RevealAt(const Text& text, int64_t tMs, int msPerGlyph);

	// When the last glyph lands.  0 when there is nothing to reveal.
	int64_t RevealDurationMs(const Text& text, int msPerGlyph);
}
} // namespace artemis::fx

#endif // ARTEMIS_FX_TYPEWRITER_H
