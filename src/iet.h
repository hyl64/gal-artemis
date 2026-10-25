// iet.h - the Artemis scenario language, parsed
//
// A shipped Artemis title keeps its story in `.iet` files: Shift-JIS text with
// KAG-style tags.  The files this tree was checked against (a free, all-ages
// doujin work) use, among others:
//
//   *label                 a named block, the target of [jump] and of a macro
//   [tag]                  a command
//   [tag attr="value" ...] a command with attributes (values may be quoted,
//                          attributes may be separated by newlines)
//   plain text             what the reader sees; `@` breaks the line inside a
//                          page, and `[rp]` ends the page and waits
//   // comment  and  /* block comment */
//   [lua] ... [/lua]       a Lua block, kept as source for the [calllua] stub
//
// The parser is deliberately free of any window, font or archive knowledge: it
// turns bytes into items, and the interpreter (render/vm.cpp) decides what the
// items mean.  That is what lets `arun` inspect a real script with no stage at
// all, and it is what the self test holds to the real scene.
//
// Two things about the real data shaped this file:
//
//  * the same tag can be a macro.  `[girl]`, `[bg_b]`, `[adv]` are not engine
//    commands at all: they are `*girl`, `*bg_b`, `*adv` blocks in the game's
//    macro files, pulled in by macroadd.  The parser keeps them as ordinary
//    commands; resolving them is the interpreter's job (see Vm).
//  * the tags are not ASCII-only in their values.  Everything after decoding is
//    UTF-8, which is what CoreText, the PNG writer and the diagnostics speak.

#ifndef ARTEMIS_IET_H
#define ARTEMIS_IET_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace artemis
{
	// Shift-JIS (cp932) to UTF-8.  An empty string when the bytes are not text.
	// Implemented with CoreFoundation's cp932 decoder, so no iconv dependency.
	std::string SjisToUtf8(const uint8_t* data, size_t size);
	std::string SjisToUtf8(const std::string& bytes);

	// Whether the bytes already are UTF-8: the real scripts are Shift-JIS, but a
	// fixture this tree writes itself is UTF-8, and re-decoding that would mangle
	// it.  Returns true when every sequence is a valid UTF-8 sequence.
	bool LooksLikeUtf8(const uint8_t* data, size_t size);

	// Decode a scenario file's bytes: Shift-JIS normally, UTF-8 when it already is.
	std::string DecodeScenarioText(const uint8_t* data, size_t size);

	struct TagAttr
	{
		std::string name;
		std::string value;
	};

	// One item of a parsed script.
	struct Item
	{
		enum class Kind
		{
			Label,   // *name
			Command, // [name attr=value ...]
			Text,    // a run of text the reader sees
		};

		Kind kind = Kind::Text;
		std::string text; // the label, the command name, or the text itself
		std::vector<TagAttr> attrs;
		int line = 1; // 1-based line in the source, for diagnostics

		bool Has(const std::string& name) const;
		const TagAttr* Attr(const std::string& name) const;
		std::string AttrOr(const std::string& name, const std::string& fallback) const;
		std::string Describe() const; // "[lyc id=0 file=bgimage/x]" for reports
	};

	// A parsed .iet file.
	struct IetParser; // in the .cpp: fills the item list while it scans

	class IetScript
	{
	public:
		// Parse already-decoded UTF-8 source.  `error` is set (and false returned)
		// only for structural damage: an unterminated tag or block comment.
		static bool Parse(const std::string& utf8Source, IetScript& out, std::string& error);

		void SetName(const std::string& name) { m_name = name; }
		const std::string& name() const { return m_name; }

		const std::vector<Item>& items() const { return m_items; }
		const std::vector<std::string>& labels() const { return m_labels; }

		// The item index of a label's body (the label itself), -1 when absent.
		int LabelIndex(const std::string& label) const;

		int commandCount() const { return m_commands; }
		int textCount() const { return m_textCount; }
		int inlineTagCount() const { return m_inlineTags; }

		// Every [lua] block in the file, concatenated: the [calllua] stub reads
		// e:tag{...} calls out of it.
		const std::string& lua() const { return m_lua; }

	private:
		friend struct IetParser;
		std::string m_name;
		std::vector<Item> m_items;
		std::vector<std::string> m_labels;
		std::map<std::string, int> m_labelAt;
		std::string m_lua;
		int m_commands = 0;
		int m_textCount = 0;
		int m_inlineTags = 0;
	};

	// Variables, as the expression evaluator sees them.
	class VarLookup
	{
	public:
		virtual ~VarLookup() = default;
		virtual std::string Get(const std::string& name) const = 0;
	};

	// A tag attribute value: plain text stays as it is, `$name` becomes the
	// variable's value, `$'prefix' + name` and `$(w - h) / 2` are evaluated.
	std::string ExpandValue(const std::string& raw, const VarLookup& vars);

	// `[if estimate="..."]`: comparisons, && || !, + - * /, parentheses, string
	// literals and variable references.  `ok` reports a parse failure (which the
	// caller counts instead of guessing).
	bool EvaluateCondition(const std::string& expression, const VarLookup& vars, bool& ok);

	// The text a reader sees from a raw scenario line: inline markup for ruby and
	// line breaks is handled here.  `@` is kept for the caller, which knows where
	// a page breaks.
	std::string StripInlineMarkup(const std::string& raw, int* inlineTags = nullptr);
}

#endif // ARTEMIS_IET_H
