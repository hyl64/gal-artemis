// iet.cpp - the Scenario parser and its little expression language
//
// See iet.h for what the format is.  Two decisions worth writing down:
//
//  * text is kept exactly as the source has it, `@` included.  A page is the
//    text between two [rp] commands, so only the interpreter knows where a page
//    ends; the parser must not decide that.  Inline tags ([ruby], [rb], [rt])
//    are ordinary command items: the parser keeps the order, and the
//    interpreter appends text items to the current page in that order.  A
//    source newline only matters between two adjacent text items, which is why
//    every item carries its line number.
//
//  * the attribute reader honours quotes, because the real macro.iet has
//    `[prohibit head="!！?？)）]〕..."]`: a `]` inside a quoted value is data, not
//    the end of the tag.  The first version of this parser ended the tag there
//    and produced garbage on that line only.

#include "iet.h"

#include <CoreFoundation/CFStringEncodingExt.h>
#include <CoreFoundation/CoreFoundation.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace artemis
{
	bool LooksLikeUtf8(const uint8_t* data, size_t size)
	{
		size_t i = 0;
		bool sawMultiByte = false;
		while(i < size)
		{
			const uint8_t c = data[i];
			if(c < 0x80)
			{
				i++;
				continue;
			}
			size_t extra = 0;
			if((c & 0xE0) == 0xC0)
				extra = 1;
			else if((c & 0xF0) == 0xE0)
				extra = 2;
			else if((c & 0xF8) == 0xF0)
				extra = 3;
			else
				return false;
			if(i + extra >= size)
				return false;
			for(size_t k = 1; k <= extra; k++)
				if((data[i + k] & 0xC0) != 0x80)
					return false;
			sawMultiByte = true;
			i += extra + 1;
		}
		(void)sawMultiByte;
		return true;
	}

	std::string SjisToUtf8(const uint8_t* data, size_t size)
	{
		if(data == nullptr || size == 0)
			return std::string();
		CFStringRef text = CFStringCreateWithBytes(kCFAllocatorDefault, data, (CFIndex)size,
			kCFStringEncodingDOSJapanese, false);
		if(text == nullptr)
			return std::string();
		const CFIndex length = CFStringGetLength(text);
		const CFIndex capacity =
			CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
		std::string out((size_t)(capacity > 0 ? capacity : 1), '\0');
		std::string result;
		if(CFStringGetCString(text, &out[0], capacity, kCFStringEncodingUTF8))
			result.assign(out.c_str());
		CFRelease(text);
		return result;
	}

	std::string SjisToUtf8(const std::string& bytes)
	{
		return SjisToUtf8((const uint8_t*)bytes.data(), bytes.size());
	}

	std::string DecodeScenarioText(const uint8_t* data, size_t size)
	{
		if(LooksLikeUtf8(data, size))
			return std::string((const char*)data, size);
		const std::string decoded = SjisToUtf8(data, size);
		if(!decoded.empty())
			return decoded;
		return std::string((const char*)data, size); // last resort: raw bytes
	}

	// ---------------------------------------------------------------------------
	// items
	// ---------------------------------------------------------------------------

	bool Item::Has(const std::string& name) const { return Attr(name) != nullptr; }

	const TagAttr* Item::Attr(const std::string& name) const
	{
		for(const TagAttr& a : attrs)
			if(a.name == name)
				return &a;
		return nullptr;
	}

	std::string Item::AttrOr(const std::string& name, const std::string& fallback) const
	{
		const TagAttr* found = Attr(name);
		return found != nullptr ? found->value : fallback;
	}

	std::string Item::Describe() const
	{
		std::string out;
		switch(kind)
		{
		case Kind::Label:
			return "*" + text;
		case Kind::Text:
			out = text;
			if(out.size() > 60)
				out = out.substr(0, 57) + "...";
			return "\"" + out + "\"";
		case Kind::Command:
			break;
		}
		out = "[" + text;
		for(const TagAttr& a : attrs)
		{
			out += " ";
			out += a.name;
			if(!a.value.empty())
			{
				std::string value = a.value;
				if(value.size() > 40)
					value = value.substr(0, 37) + "...";
				out += "=" + value;
			}
		}
		out += "]";
		return out;
	}

	// ---------------------------------------------------------------------------
	// parsing
	// ---------------------------------------------------------------------------
	namespace
	{
		bool IsNameChar(char c)
		{
			return std::isalnum((unsigned char)c) != 0 || c == '_' || c == ':' || c == '.' ||
				c == '-';
		}

	}

	struct IetParser
	{
			const std::string& source;
			size_t at = 0;
			int line = 1;
			IetScript& out;
			std::string& error;
			bool failed = false;
			std::string text;
			int textLine = 1;

			IetParser(const std::string& sourceUtf8, IetScript& script, std::string& errorText)
				: source(sourceUtf8), out(script), error(errorText)
			{
			}

			void Advance(size_t count)
			{
				for(size_t k = 0; k < count && at < source.size(); k++)
				{
					if(source[at] == '\n')
						line++;
					at++;
				}
			}

			// A real script line is a few hundred bytes.  One hundred megabytes on a
			// single line is not a script, and accepting it costs seconds and half a
			// gigabyte before anything is drawn, so it is refused up front.
			static constexpr size_t kMaxLineBytes = 1u << 20;

			bool CheckLineLengths()
			{
				size_t start = 0;
				for(size_t i = 0; i <= source.size(); i++)
				{
					if(i < source.size() && source[i] != '\n')
						continue;
					if(i - start > kMaxLineBytes)
					{
						line = 1;
						error = "line longer than 1 MiB: a script line is not this long";
						return false;
					}
					start = i + 1;
				}
				return true;
			}

			void Fail(const std::string& why)
			{
				if(failed)
					return;
				failed = true;
				char buffer[512];
				snprintf(buffer, sizeof buffer, "line %d: %s", line, why.c_str());
				error = buffer;
			}

			void FlushText()
			{
				if(text.empty())
					return;
				Item item;
				item.kind = Item::Kind::Text;
				item.text = text;
				item.line = textLine;
				out.m_items.push_back(item);
				out.m_textCount++;
				text.clear();
			}

			// a `*label` at the start of a line: everything after the name on that
			// line is a comment
			void ParseLabel()
			{
				Advance(1); // '*'
				std::string name;
				while(at < source.size() && IsNameChar(source[at]))
				{
					name += source[at];
					Advance(1);
				}
				while(at < source.size() && source[at] != '\n')
					Advance(1);
				if(name.empty())
					return;
				Item item;
				item.kind = Item::Kind::Label;
				item.text = name;
				item.line = line;
				const int index = (int)out.m_items.size();
				out.m_items.push_back(item);
				if(out.m_labelAt.find(name) == out.m_labelAt.end())
				{
					out.m_labelAt[name] = index;
					out.m_labels.push_back(name);
				}
			}

			bool InlineTag(const std::string& name) const
			{
				return name == "ruby" || name == "rb" || name == "rt" || name == "ruby2" ||
					name == "/ruby" || name == "/rb" || name == "/rt" || name == "/ruby2";
			}

			// [lua] ... [/lua]: the body is Lua, not tags, so it is taken raw
			void ParseLuaBlock()
			{
				const size_t bodyAt = at;
				size_t found = source.find("[/lua]", bodyAt);
				if(found == std::string::npos)
				{
					// a file may open a lua block and never close it inside the same
					// chunk; take the rest rather than lose it
					out.m_lua += source.substr(bodyAt);
					out.m_lua += "\n";
					Advance(source.size() - at);
					return;
				}
				out.m_lua += source.substr(bodyAt, found - bodyAt);
				out.m_lua += "\n";
				Advance(found - at);
				Advance(6); // "[/lua]"
			}

			void ParseTag()
			{
				const int tagLine = line;
				Advance(1); // '['
				std::string name;
				while(at < source.size() && (IsNameChar(source[at]) || source[at] == '/' ||
					source[at] == '*'))
				{
					name += source[at];
					Advance(1);
				}
				if(name == "lua")
				{
					ParseLuaBlock();
					return;
				}
				std::vector<TagAttr> attrs;
				bool closed = false;
				while(at < source.size())
				{
					while(at < source.size() && std::isspace((unsigned char)source[at]) != 0)
						Advance(1);
					if(at >= source.size())
						break;
					if(source[at] == ']')
					{
						Advance(1);
						closed = true;
						break;
					}
					std::string attrName;
					while(at < source.size() && IsNameChar(source[at]))
					{
						attrName += source[at];
						Advance(1);
					}
					if(attrName.empty())
					{
						Advance(1); // punctuation we do not understand: skip, do not loop
						continue;
					}
					while(at < source.size() && std::isspace((unsigned char)source[at]) != 0)
						Advance(1);
					std::string attrValue;
					if(at < source.size() && source[at] == '=')
					{
						Advance(1);
						while(at < source.size() && std::isspace((unsigned char)source[at]) != 0)
							Advance(1);
						if(at < source.size() && (source[at] == '"' || source[at] == '\''))
						{
							const char quote = source[at];
							Advance(1);
							while(at < source.size() && source[at] != quote)
							{
								if(source[at] == '\\' && at + 1 < source.size())
								{
									Advance(1);
									attrValue += source[at];
									Advance(1);
									continue;
								}
								attrValue += source[at];
								Advance(1);
							}
							if(at < source.size())
								Advance(1); // closing quote
						}
						else
						{
							while(at < source.size() && source[at] != ']' &&
								std::isspace((unsigned char)source[at]) == 0)
							{
								attrValue += source[at];
								Advance(1);
							}
						}
					}
					TagAttr attr;
					attr.name = attrName;
					attr.value = attrValue;
					attrs.push_back(attr);
				}
				if(!closed && at >= source.size())
				{
					Fail("unterminated tag [" + name);
					return;
				}
				Item item;
				item.kind = Item::Kind::Command;
				item.text = name;
				item.attrs = std::move(attrs);
				item.line = tagLine;
				if(InlineTag(name))
					out.m_inlineTags++;
				out.m_commands++;
				out.m_items.push_back(std::move(item));
			}

			void Run()
			{
				while(at < source.size() && !failed)
				{
					const char c = source[at];
					if(c == '/' && at + 1 < source.size() && source[at + 1] == '/')
					{
						while(at < source.size() && source[at] != '\n')
							Advance(1);
						continue;
					}
					if(c == '/' && at + 1 < source.size() && source[at + 1] == '*')
					{
						const size_t end = source.find("*/", at + 2);
						if(end == std::string::npos)
						{
							Fail("unterminated block comment");
							return;
						}
						Advance(end - at + 2);
						continue;
					}
					if(c == '\n')
					{
						FlushText();
						Advance(1);
						textLine = line;
						continue;
					}
					if(c == '\r')
					{
						Advance(1);
						continue;
					}
					if(c == '[')
					{
						FlushText();
						ParseTag();
						continue;
					}
					if(c == '*' && text.empty())
					{
						// a label only counts at the start of a line
						bool atLineStart = true;
						for(size_t k = at; k > 0; k--)
						{
							const char prev = source[k - 1];
							if(prev == '\n')
								break;
							if(!std::isspace((unsigned char)prev))
							{
								atLineStart = false;
								break;
							}
						}
						if(atLineStart)
						{
							ParseLabel();
							continue;
						}
					}
					if(text.empty())
						textLine = line;
					text += c;
					Advance(1);
				}
				FlushText();
			}
	};

	bool IetScript::Parse(const std::string& utf8Source, IetScript& out, std::string& error)
	{
		out = IetScript();
		IetParser parser(utf8Source, out, error);
		if(!parser.CheckLineLengths())
			return false;
		parser.Run();
		if(parser.failed)
			return false;
		error.clear();
		return true;
	}

	int IetScript::LabelIndex(const std::string& label) const
	{
		std::map<std::string, int>::const_iterator found = m_labelAt.find(label);
		return found == m_labelAt.end() ? -1 : found->second;
	}

	// ---------------------------------------------------------------------------
	// values and conditions
	// ---------------------------------------------------------------------------
	namespace
	{
		struct Value
		{
			bool numeric = false;
			double number = 0.0;
			std::string text;

			std::string AsString() const
			{
				if(!numeric)
					return text;
				if(std::fabs(number - std::floor(number)) < 1e-9 &&
					std::fabs(number) < 9.0e15)
				{
					char buffer[64];
					snprintf(buffer, sizeof buffer, "%lld", (long long)number);
					return buffer;
				}
				char buffer[64];
				snprintf(buffer, sizeof buffer, "%g", number);
				return buffer;
			}
		};

		bool ToNumber(const std::string& text, double& out)
		{
			if(text.empty())
				return false;
			char* end = nullptr;
			const double value = strtod(text.c_str(), &end);
			if(end == nullptr || *end != '\0')
				return false;
			out = value;
			return true;
		}

		bool Truthy(const Value& value)
		{
			if(value.numeric)
				return value.number != 0.0;
			if(value.text.empty() || value.text == "0" || value.text == "false")
				return false;
			return true;
		}

		struct Expr
		{
			const std::string& source;
			size_t at = 0;
			const VarLookup& vars;
			bool ok = true;

			Expr(const std::string& text, const VarLookup& lookup) : source(text), vars(lookup) {}

			void Skip()
			{
				while(at < source.size() && std::isspace((unsigned char)source[at]) != 0)
					at++;
			}

			bool Match(const char* op)
			{
				Skip();
				const size_t length = strlen(op);
				if(source.compare(at, length, op) == 0)
				{
					at += length;
					return true;
				}
				return false;
			}

			Value Number(double value)
			{
				Value out;
				out.numeric = true;
				out.number = value;
				return out;
			}

			Value Variable(const std::string& name)
			{
				Value out;
				out.text = vars.Get(name);
				double number = 0;
				if(ToNumber(out.text, number))
				{
					out.numeric = true;
					out.number = number;
				}
				return out;
			}

			Value ParseExpression() { return ParseOr(); }

			Value ParseOr()
			{
				Value left = ParseAnd();
				while(Match("||"))
				{
					Value right = ParseAnd();
					left = Number((Truthy(left) || Truthy(right)) ? 1 : 0);
				}
				return left;
			}

			Value ParseAnd()
			{
				Value left = ParseCompare();
				while(Match("&&"))
				{
					Value right = ParseCompare();
					left = Number((Truthy(left) && Truthy(right)) ? 1 : 0);
				}
				return left;
			}

			Value ParseCompare()
			{
				Value left = ParseAdd();
				for(;;)
				{
					Skip();
					if(Match("=="))
					{
						Value right = ParseAdd();
						left = Number(Equal(left, right) ? 1 : 0);
						continue;
					}
					if(Match("!="))
					{
						Value right = ParseAdd();
						left = Number(Equal(left, right) ? 0 : 1);
						continue;
					}
					if(Match(">="))
					{
						Value right = ParseAdd();
						left = Number(Compare(left, right) >= 0 ? 1 : 0);
						continue;
					}
					if(Match("<="))
					{
						Value right = ParseAdd();
						left = Number(Compare(left, right) <= 0 ? 1 : 0);
						continue;
					}
					if(Match(">"))
					{
						Value right = ParseAdd();
						left = Number(Compare(left, right) > 0 ? 1 : 0);
						continue;
					}
					if(Match("<"))
					{
						Value right = ParseAdd();
						left = Number(Compare(left, right) < 0 ? 1 : 0);
						continue;
					}
					break;
				}
				return left;
			}

			// An unset variable is 0 in the real scripts: first.iet runs its
			// "first time" block on [if estimate="$g.firsttime == 0"], which only
			// works if an empty variable compares equal to zero.  Two strings that
			// are not numbers still compare as strings.
			static bool Number(const Value& value, double& out)
			{
				if(value.numeric)
				{
					out = value.number;
					return true;
				}
				if(value.text.empty())
				{
					out = 0.0;
					return true;
				}
				return ToNumber(value.text, out);
			}

			static bool Equal(const Value& a, const Value& b)
			{
				double left = 0, right = 0;
				if(Number(a, left) && Number(b, right))
					return std::fabs(left - right) < 1e-9;
				return a.AsString() == b.AsString();
			}

			static int Compare(const Value& a, const Value& b)
			{
				double left = 0, right = 0;
				if(Number(a, left) && Number(b, right))
					return left < right ? -1 : (left > right ? 1 : 0);
				const std::string as = a.AsString(), bs = b.AsString();
				return as < bs ? -1 : (as > bs ? 1 : 0);
			}

			Value ParseAdd()
			{
				Value left = ParseMultiply();
				for(;;)
				{
					Skip();
					if(at < source.size() && source[at] == '+' && (at + 1 >= source.size() ||
						source[at + 1] != '+'))
					{
						at++;
						Value right = ParseMultiply();
						if(left.numeric && right.numeric)
							left = Number(left.number + right.number);
						else
						{
							Value joined;
							joined.text = left.AsString() + right.AsString();
							left = joined;
						}
						continue;
					}
					if(at < source.size() && source[at] == '-' && (at + 1 >= source.size() ||
						source[at + 1] != '-'))
					{
						at++;
						Value right = ParseMultiply();
						left = Number((left.numeric ? left.number : 0) -
							(right.numeric ? right.number : 0));
						continue;
					}
					break;
				}
				return left;
			}

			Value ParseMultiply()
			{
				Value left = ParseUnary();
				for(;;)
				{
					Skip();
					if(at < source.size() && source[at] == '*')
					{
						at++;
						Value right = ParseUnary();
						left = Number((left.numeric ? left.number : 0) *
							(right.numeric ? right.number : 0));
						continue;
					}
					if(at < source.size() && source[at] == '/')
					{
						at++;
						Value right = ParseUnary();
						const double divisor = right.numeric ? right.number : 0;
						left = Number(divisor == 0 ? 0 : (left.numeric ? left.number : 0) / divisor);
						continue;
					}
					break;
				}
				return left;
			}

			Value ParseUnary()
			{
				Skip();
				if(at < source.size() && source[at] == '!')
				{
					at++;
					return Number(Truthy(ParseUnary()) ? 0 : 1);
				}
				if(at < source.size() && source[at] == '-')
				{
					at++;
					Value inner = ParseUnary();
					return Number(-(inner.numeric ? inner.number : 0));
				}
				if(at < source.size() && source[at] == '+')
				{
					at++;
					return ParseUnary();
				}
				return ParsePrimary();
			}

			Value ParsePrimary()
			{
				Skip();
				if(at >= source.size())
				{
					ok = false;
					return Value();
				}
				const char c = source[at];
				if(c == '(')
				{
					at++;
					Value inner = ParseExpression();
					Skip();
					if(at < source.size() && source[at] == ')')
						at++;
					else
						ok = false;
					return inner;
				}
				if(c == '$')
				{
					at++;
					return ParsePrimary();
				}
				if(c == '"' || c == '\'')
				{
					const char quote = c;
					at++;
					std::string literal;
					while(at < source.size() && source[at] != quote)
					{
						if(source[at] == '\\' && at + 1 < source.size())
						{
							at++;
							literal += source[at];
							at++;
							continue;
						}
						literal += source[at];
						at++;
					}
					if(at < source.size())
						at++;
					Value value;
					value.text = literal;
					double number = 0;
					if(ToNumber(literal, number))
					{
						value.numeric = true;
						value.number = number;
					}
					return value;
				}
				if(std::isdigit((unsigned char)c) != 0 || c == '.')
				{
					size_t start = at;
					while(at < source.size() && (std::isdigit((unsigned char)source[at]) != 0 ||
						source[at] == '.'))
						at++;
					return Number(strtod(source.substr(start, at - start).c_str(), nullptr));
				}
				if(std::isalpha((unsigned char)c) != 0 || c == '_')
				{
					size_t start = at;
					while(at < source.size() && (std::isalnum((unsigned char)source[at]) != 0 ||
						source[at] == '_' || source[at] == '.'))
						at++;
					return Variable(source.substr(start, at - start));
				}
				ok = false;
				return Value();
			}
		};
	}

	std::string ExpandValue(const std::string& raw, const VarLookup& vars)
	{
		if(raw.empty() || raw[0] != '$')
			return raw;
		std::string rest = raw.substr(1);
		if(!rest.empty() && rest[0] == '$')
			rest = rest.substr(1); // $$name: the value names another variable
		Expr expr(rest, vars);
		Value value = expr.ParseExpression();
		expr.Skip();
		if(!expr.ok || expr.at != rest.size())
		{
			// not an expression after all: a plain `$name` lookup, the common case
			std::string name;
			for(char c : rest)
			{
				if(std::isalnum((unsigned char)c) == 0 && c != '_' && c != '.')
					break;
				name += c;
			}
			return name.empty() ? raw : vars.Get(name);
		}
		return value.AsString();
	}

	bool EvaluateCondition(const std::string& expression, const VarLookup& vars, bool& ok)
	{
		Expr expr(expression, vars);
		Value value = expr.ParseExpression();
		expr.Skip();
		ok = expr.ok && expr.at == expression.size();
		if(!ok)
			return false;
		return Truthy(value);
	}

	std::string StripInlineMarkup(const std::string& raw, int* inlineTags)
	{
		std::string out;
		int count = 0;
		size_t at = 0;
		while(at < raw.size())
		{
			if(raw[at] != '[')
			{
				out += raw[at];
				at++;
				continue;
			}
			size_t close = at + 1;
			bool quoted = false;
			char quote = 0;
			for(; close < raw.size(); close++)
			{
				const char c = raw[close];
				if(quoted)
				{
					if(c == quote)
						quoted = false;
					continue;
				}
				if(c == '"' || c == '\'')
				{
					quoted = true;
					quote = c;
					continue;
				}
				if(c == ']')
					break;
			}
			if(close >= raw.size())
			{
				out += raw.substr(at);
				break;
			}
			std::string name;
			for(size_t k = at + 1; k < close && IsNameChar(raw[k]); k++)
				name += raw[k];
			if(name == "ruby" || name == "rb" || name == "rt" || name == "ruby2" ||
				name == "/ruby" || name == "/rb" || name == "/rt" || name == "/ruby2")
				count++;
			else
				out += raw.substr(at, close - at + 1);
			at = close + 1;
		}
		if(inlineTags != nullptr)
			*inlineTags = count;
		return out;
	}
}
