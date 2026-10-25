// tags.h - the remaining script tags: registered, and the two that do work
//
// The corpus writes these far more often than any effect:
//
//   cacheclear   292      no attributes
//   exfont       266      size="f4", or nothing (230 bare + 36 sized)
//   savetitle     55      24 distinct texts, e.g. "LET'S GO IMPRISONMENT THE PRISON!"
//   movie          5      file="op"/"ed2"/"cutin_shuichiro"/..., cancel=1, bg="black"
//   タイトル       2      no attributes
//   autoplay       2      one bare, one mode="stop"
//   skipstop       1      no attributes
//
// Two of them are real behaviour, not a registry line:
//
//   savetitle   stores the save-slot title.  It is truncated to 96 *bytes* at a
//               UTF-8 code-point boundary, never inside a character: a naive
//               byte cut of "監獄の詩" leaves half a glyph and the save list
//               draws a replacement box.  The book keeps the history so a test
//               can feed the 55 real titles through it (24 distinct).
//   cacheclear  drops every cached texture and reports how many and how many
//               bytes it freed.  That is the whole tag: no attribute, no state
//               beyond the cache.
//
// `movie` parses its path and says playback is not implemented - it must not be
// reported as an unknown tag.  The other four are registered with a defined
// behaviour (return to title / leave skip / stop autoplay / extra font face)
// whose execution belongs to the player, not to this layer.

#ifndef ARTEMIS_FX_TAGS_H
#define ARTEMIS_FX_TAGS_H

#include "tween.h" // AttrNameValue

#include <cstddef>
#include <string>
#include <vector>

namespace artemis
{
namespace fx
{
	enum class TagKind
	{
		Movie,
		Title,
		Autoplay,
		SkipStop,
		CacheClear,
		Exfont,
		SaveTitle,
	};

	enum class TagStatus
	{
		Implemented, // this layer does the work
		Registered,  // parsed, with a defined behaviour for the player
		NotPlayable, // parsed, deliberately not performed here
	};

	const char* TagStatusName(TagStatus status);

	struct TagSpec
	{
		std::string name;
		TagKind kind = TagKind::Movie;
		TagStatus status = TagStatus::Registered;
		std::string behaviour;
	};

	const std::vector<TagSpec>& TagRegistry();
	const TagSpec* FindTag(const std::string& name);

	struct MovieTag
	{
		std::string file; // a path without extension: "op", "cutin_rin"
		std::string bg;   // "black" when the video sits on a black field
		bool cancel = false;
		bool hasFile = false;
		bool playbackImplemented() const { return false; }
	};
	bool ParseMovieTag(const std::vector<AttrNameValue>& attrs, MovieTag& out,
		std::string& note);

	struct AutoplayTag
	{
		std::string mode; // "stop" in the corpus; empty = start
		bool stop = false;
		bool hasMode = false;
	};
	bool ParseAutoplayTag(const std::vector<AttrNameValue>& attrs, AutoplayTag& out,
		std::string& note);

	struct ExfontTag
	{
		std::string size; // "f4" in the corpus; empty = default face
		bool hasSize = false;
	};
	bool ParseExfontTag(const std::vector<AttrNameValue>& attrs, ExfontTag& out,
		std::string& note);

	struct SaveTitleTag
	{
		std::string text;
		bool valid = false;
	};
	bool ParseSaveTitleTag(const std::vector<AttrNameValue>& attrs, SaveTitleTag& out,
		std::string& note);

	// True when `text` is well-formed UTF-8 (no truncated or stray sequences).
	bool IsValidUtf8(const std::string& text);

	// The longest prefix of `text` of at most `maxBytes` bytes that does not cut
	// a code point in half.  A character that would not fit is dropped whole.
	std::string TruncateUtf8(const std::string& text, size_t maxBytes);

	// The save-title stack behind [savetitle].
	class SaveTitleBook
	{
	public:
		static constexpr size_t kMaxTitleBytes = 96;

		// Rejects an empty title; otherwise truncates (UTF-8 safe), records it and
		// makes it current.  Returns false only when nothing usable was left.
		bool Set(const std::string& text);

		const std::string& Current() const { return m_current; }
		const std::vector<std::string>& History() const { return m_history; }
		size_t Sets() const { return m_history.size(); }
		size_t UniqueTitles() const;
		void Clear();

	private:
		std::string m_current;
		std::vector<std::string> m_history;
	};

	// The texture cache behind [cacheclear].  It is deliberately a real store:
	// insert in tests, clear, and the freed count/bytes are the tag's report.
	class TextureCache
	{
	public:
		struct ClearResult
		{
			size_t textures = 0;
			size_t bytes = 0;
		};

		void Insert(const std::string& name, size_t bytes);
		bool Has(const std::string& name) const;
		size_t Count() const { return m_entries.size(); }
		size_t Bytes() const { return m_bytes; }
		ClearResult Clear();

	private:
		struct Entry
		{
			std::string name;
			size_t bytes = 0;
		};
		std::vector<Entry> m_entries;
		size_t m_bytes = 0;
	};
}
} // namespace artemis::fx

#endif // ARTEMIS_FX_TAGS_H
