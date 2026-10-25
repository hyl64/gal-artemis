// pages.h - cutting a parsed script into the pages a reader sees
//
// A typewriter needs two things: a page, and a clock.  The clock is
// `$g.msgspeed` (typewriter.h); the page is this file.  The rule is not
// invented here - it is the rule `render/vm.cpp` already runs, which is what
// makes `agame`'s first screen and `fxtool`'s page N the same page:
//
//   * text items accumulate into one page buffer;
//   * a new text item that starts on a later source line gets a '\n' in front
//     of it (three Text items can share one line - see the ruby sample below);
//   * `@` inside a text item is a line break, not a character;
//   * a page ends at `[rp]`, the engine's own page break (409 in the real
//     scene); the buffer is trimmed and an empty page does *not* wait for a
//     click - that is how a macro file clears the message layer;
//   * inline `ruby`/`rb`/`rt` markup contributes no text.  The real script says
//     `[ruby text="おりひめ"]織姫[/ruby]`, and the *base* (織姫) is the body; the
//     annotation is furigana, so it must not consume a typewriter step.
//
// The `@`/text scan happens on the decoded UTF-8, never on the Shift-JIS bytes:
// in cp932 the second byte of a two-byte character is often 0x40, which is '@'.
// Counting '@' in the raw file gives a bigger number than the text has, so this
// file only ever looks at code points.
//
// What is deliberately *not* modelled: which message layer a text item targets.
// `[chgmsg]` drives that, the real scene never uses it, and resolving macros is
// the interpreter's job.  A page feed is a pure function of the item stream so
// that it can be tested without a window.

#ifndef ARTEMIS_FX_PAGES_H
#define ARTEMIS_FX_PAGES_H

#include <string>
#include <vector>

namespace artemis
{
namespace fx
{
	// The parts of artemis::Item a page splitter needs; fxtool adapts the parsed
	// script to this, so nothing here depends on the .iet parser or on a stage.
	struct FeedItem
	{
		enum class Kind
		{
			Text,
			Command,
		};

		Kind kind = Kind::Text;
		std::string name; // command name; empty for text
		std::string data; // the `data` attribute, for [print]
		std::string text; // the text run itself (decoded UTF-8)
		int line = 0;     // 1-based source line

		static FeedItem Text_(const std::string& text, int line)
		{
			FeedItem item;
			item.kind = Kind::Text;
			item.text = text;
			item.line = line;
			return item;
		}

		static FeedItem Command_(const std::string& name, int line,
			const std::string& data = std::string())
		{
			FeedItem item;
			item.kind = Kind::Command;
			item.name = name;
			item.data = data;
			item.line = line;
			return item;
		}
	};

	struct Page
	{
		std::string text;   // UTF-8, '\n' between lines, trimmed
		int line = 0;       // source line of the tag that closed the page
		int glyphStops = 0; // filled by the caller (typewriter.h says what a glyph is)
	};

	struct PageFeedStats
	{
		int pages = 0;
		int emptyDropped = 0;   // [rp] with nothing on screen
		int inlineDropped = 0;  // ruby/rb/rt markup
		int printed = 0;        // [print] text appended
		int otherCommands = 0;
	};

	// Whether a command name is one of the inline ruby marks (case-insensitive).
	bool IsInlineMarkup(const std::string& name);

	// Whether a command ends a page: `rp`, and `rp2` which the real scene also
	// uses (11 times).
	bool IsPageBreak(const std::string& name);

	std::vector<Page> SplitPages(const std::vector<FeedItem>& items,
		PageFeedStats* stats = nullptr);

	// The same trimming `vm.cpp` does before it decides a page is empty.
	std::string TrimPageText(const std::string& text);
}
} // namespace artemis::fx

#endif // ARTEMIS_FX_PAGES_H
