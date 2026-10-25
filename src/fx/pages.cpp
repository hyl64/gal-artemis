// pages.cpp - see pages.h

#include "fx/pages.h"

#include <cctype>

namespace artemis
{
namespace fx
{
	namespace
	{
		std::string Lower(const std::string& s)
		{
			std::string out = s;
			for(char& c : out)
				c = (char)std::tolower((unsigned char)c);
			return out;
		}

		bool IsSpace(char c)
		{
			return c == ' ' || c == '\t' || c == '\r' || c == '\n';
		}
	}

	std::string TrimPageText(const std::string& text)
	{
		size_t start = 0, end = text.size();
		while(start < end && IsSpace(text[start]))
			start++;
		while(end > start && IsSpace(text[end - 1]))
			end--;
		return text.substr(start, end - start);
	}

	bool IsInlineMarkup(const std::string& name)
	{
		const std::string lower = Lower(name);
		return lower == "ruby" || lower == "ruby2" || lower == "rb" || lower == "rt" ||
			lower == "/ruby" || lower == "/ruby2" || lower == "/rb" || lower == "/rt";
	}

	bool IsPageBreak(const std::string& name)
	{
		const std::string lower = Lower(name);
		return lower == "rp" || lower == "rp2";
	}

	std::vector<Page> SplitPages(const std::vector<FeedItem>& items,
		PageFeedStats* stats)
	{
		PageFeedStats local;
		PageFeedStats& counts = stats != nullptr ? *stats : local;

		std::vector<Page> pages;
		std::string buffer;
		int lastTextLine = 0;

		for(const FeedItem& item : items)
		{
			if(item.kind == FeedItem::Kind::Text)
			{
				// a text item on a new source line starts a new display line - but
				// three Text items can share one line (`[ruby]織姫[/ruby]と`), and
				// those must not be split apart
				if(lastTextLine > 0 && item.line > lastTextLine && !buffer.empty() &&
					buffer.back() != '\n')
					buffer += '\n';
				lastTextLine = item.line;
				for(size_t i = 0; i < item.text.size(); i++)
				{
					// walk code points so a '@' that is part of a multi-byte
					// character is never mistaken for a break
					const unsigned char c = (unsigned char)item.text[i];
					if(c < 0x80)
					{
						buffer += (c == '@') ? '\n' : (char)c;
						continue;
					}
					size_t length = 1;
					if((c & 0xE0) == 0xC0)
						length = 2;
					else if((c & 0xF0) == 0xE0)
						length = 3;
					else if((c & 0xF8) == 0xF0)
						length = 4;
					if(i + length > item.text.size())
						length = 1;
					buffer.append(item.text, i, length);
					i += length - 1;
				}
				continue;
			}

			const std::string name = Lower(item.name);
			if(IsInlineMarkup(name))
			{
				counts.inlineDropped++;
				continue;
			}
			if(IsPageBreak(name))
			{
				const std::string text = TrimPageText(buffer);
				if(text.empty())
					counts.emptyDropped++;
				else
				{
					Page page;
					page.text = text;
					page.line = item.line;
					pages.push_back(page);
					counts.pages++;
				}
				buffer.clear();
				lastTextLine = 0;
				continue;
			}
			if(name == "print")
			{
				// `[print data="..."]` puts text on the page without a new line
				buffer += item.data;
				counts.printed++;
				continue;
			}
			counts.otherCommands++;
		}

		// A file may end mid-page; vm.cpp would leave that text unwaited-for, but
		// a standalone reader should not throw the last line away.
		const std::string tail = TrimPageText(buffer);
		if(!tail.empty())
		{
			Page page;
			page.text = tail;
			page.line = items.empty() ? 0 : items.back().line;
			pages.push_back(page);
			counts.pages++;
		}
		return pages;
	}
}
} // namespace artemis::fx
