// tags.cpp - see tags.h

#include "fx/tags.h"

#include <algorithm>
#include <cstdint>
#include <set>

namespace artemis
{
namespace fx
{
	namespace
	{
		std::string AttrOf(const std::vector<AttrNameValue>& attrs, const std::string& name)
		{
			for(const AttrNameValue& attr : attrs)
				if(attr.name == name)
					return attr.value;
			return std::string();
		}

		bool HasAttr(const std::vector<AttrNameValue>& attrs, const std::string& name)
		{
			for(const AttrNameValue& attr : attrs)
				if(attr.name == name)
					return true;
			return false;
		}

		size_t Utf8Length(unsigned char lead)
		{
			if(lead < 0x80)
				return 1;
			if((lead & 0xE0) == 0xC0)
				return 2;
			if((lead & 0xF0) == 0xE0)
				return 3;
			if((lead & 0xF8) == 0xF0)
				return 4;
			return 0; // continuation byte or an illegal lead
		}

		bool Utf8SequenceAt(const std::string& text, size_t at, size_t length)
		{
			if(length == 0 || at + length > text.size())
				return false;
			for(size_t k = 1; k < length; k++)
				if(((unsigned char)text[at + k] & 0xC0) != 0x80)
					return false;
			if(length == 1)
				return true;
			uint32_t code = (unsigned char)text[at] & (0xFFu >> (length + 1));
			for(size_t k = 1; k < length; k++)
				code = (code << 6) | ((unsigned char)text[at + k] & 0x3F);
			static const uint32_t minimum[5] = { 0, 0, 0x80, 0x800, 0x10000 };
			if(code < minimum[length])
				return false; // overlong
			if(code >= 0xD800 && code <= 0xDFFF)
				return false; // surrogate half
			if(code > 0x10FFFF)
				return false;
			return true;
		}
	}

	const char* TagStatusName(TagStatus status)
	{
		switch(status)
		{
		case TagStatus::Implemented: return "implemented";
		case TagStatus::Registered: return "registered";
		case TagStatus::NotPlayable: return "not-playable";
		}
		return "?";
	}

	const std::vector<TagSpec>& TagRegistry()
	{
		static const std::vector<TagSpec> registry = {
			{ "movie", TagKind::Movie, TagStatus::NotPlayable,
				"parses file/bg/cancel; video playback is not implemented" },
			{ "タイトル", TagKind::Title, TagStatus::Registered,
				"returns to the title screen; the player owns the transition" },
			{ "autoplay", TagKind::Autoplay, TagStatus::Registered,
				"mode=stop stops autoplay; otherwise starts it" },
			{ "skipstop", TagKind::SkipStop, TagStatus::Registered,
				"leaves skip mode" },
			{ "cacheclear", TagKind::CacheClear, TagStatus::Implemented,
				"drops every cached texture and reports the freed count and bytes" },
			{ "exfont", TagKind::Exfont, TagStatus::Registered,
				"size selects an extra font face; not applied here" },
			{ "savetitle", TagKind::SaveTitle, TagStatus::Implemented,
				"stores a save-slot title, truncated to 96 bytes on a UTF-8 boundary" },
		};
		return registry;
	}

	const TagSpec* FindTag(const std::string& name)
	{
		const std::vector<TagSpec>& registry = TagRegistry();
		for(const TagSpec& spec : registry)
			if(spec.name == name)
				return &spec;
		return nullptr;
	}

	bool ParseMovieTag(const std::vector<AttrNameValue>& attrs, MovieTag& out,
		std::string& note)
	{
		out = MovieTag{};
		out.file = AttrOf(attrs, "file");
		out.bg = AttrOf(attrs, "bg");
		out.hasFile = !out.file.empty();
		const std::string cancel = AttrOf(attrs, "cancel");
		out.cancel = cancel == "1" || cancel == "true";
		if(!out.hasFile)
		{
			note = "movie tag without a file: nothing to play";
			return false;
		}
		note = "path parsed; playback is not implemented";
		return true;
	}

	bool ParseAutoplayTag(const std::vector<AttrNameValue>& attrs, AutoplayTag& out,
		std::string& note)
	{
		out = AutoplayTag{};
		out.hasMode = HasAttr(attrs, "mode");
		out.mode = AttrOf(attrs, "mode");
		out.stop = out.mode == "stop";
		if(out.hasMode && !out.stop && out.mode != "start")
			note = "autoplay mode \"" + out.mode + "\" is neither start nor stop";
		return true;
	}

	bool ParseExfontTag(const std::vector<AttrNameValue>& attrs, ExfontTag& out,
		std::string& note)
	{
		out = ExfontTag{};
		out.hasSize = HasAttr(attrs, "size");
		out.size = AttrOf(attrs, "size");
		if(!out.hasSize)
			note = "no size: the default face";
		return true;
	}

	bool ParseSaveTitleTag(const std::vector<AttrNameValue>& attrs, SaveTitleTag& out,
		std::string& note)
	{
		out = SaveTitleTag{};
		out.text = AttrOf(attrs, "text");
		out.valid = !out.text.empty();
		if(!out.valid)
			note = "savetitle tag without text";
		return out.valid;
	}

	bool IsValidUtf8(const std::string& text)
	{
		size_t at = 0;
		while(at < text.size())
		{
			const size_t length = Utf8Length((unsigned char)text[at]);
			if(!Utf8SequenceAt(text, at, length))
				return false;
			at += length;
		}
		return true;
	}

	std::string TruncateUtf8(const std::string& text, size_t maxBytes)
	{
		if(text.size() <= maxBytes)
			return text;
		size_t at = 0;
		while(at < text.size())
		{
			const size_t length = Utf8Length((unsigned char)text[at]);
			if(!Utf8SequenceAt(text, at, length))
				break; // do not copy a malformed byte into the title
			if(at + length > maxBytes)
				break; // the whole character would not fit: drop it
			at += length;
		}
		return text.substr(0, at);
	}

	bool SaveTitleBook::Set(const std::string& text)
	{
		if(text.empty())
			return false;
		const std::string stored = TruncateUtf8(text, kMaxTitleBytes);
		if(stored.empty())
			return false;
		m_current = stored;
		m_history.push_back(stored);
		return true;
	}

	size_t SaveTitleBook::UniqueTitles() const
	{
		std::set<std::string> unique(m_history.begin(), m_history.end());
		return unique.size();
	}

	void SaveTitleBook::Clear()
	{
		m_current.clear();
		m_history.clear();
	}

	void TextureCache::Insert(const std::string& name, size_t bytes)
	{
		for(Entry& entry : m_entries)
		{
			if(entry.name == name)
			{
				m_bytes -= entry.bytes;
				entry.bytes = bytes;
				m_bytes += bytes;
				return;
			}
		}
		m_entries.push_back(Entry{ name, bytes });
		m_bytes += bytes;
	}

	bool TextureCache::Has(const std::string& name) const
	{
		for(const Entry& entry : m_entries)
			if(entry.name == name)
				return true;
		return false;
	}

	TextureCache::ClearResult TextureCache::Clear()
	{
		ClearResult result;
		result.textures = m_entries.size();
		result.bytes = m_bytes;
		m_entries.clear();
		m_bytes = 0;
		return result;
	}
}
} // namespace artemis::fx
