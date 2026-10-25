// lua.cpp - the table parser described in lua.h

#include "lua.h"

#include <cstdlib>
#include <cstring>

namespace artemis
{
	const LuaValue* LuaValue::field(const std::string& name) const
	{
		const LuaValue* found = nullptr;
		for(const auto& e : entries)
			if(e.first == name)
				found = &e.second; // last write wins, as in a Lua table
		return found;
	}

	const std::string* LuaValue::firstString() const
	{
		for(const LuaValue& v : items)
			if(v.kind == LuaValue::Kind::String)
				return &v.text;
		return nullptr;
	}

	std::string LuaValue::summary() const
	{
		switch(kind)
		{
		case Kind::Nil: return "nil";
		case Kind::Boolean: return boolean ? "true" : "false";
		case Kind::Integer: return "integer " + std::to_string(integer);
		case Kind::Number: return "number";
		case Kind::String: return "string";
		case Kind::Identifier: return "identifier " + text;
		case Kind::Object: return "object with " + std::to_string(entries.size())
			+ " named field(s) and " + std::to_string(items.size()) + " array item(s)";
		}
		return "?";
	}

	const char* Name(LuaValue::Kind kind)
	{
		switch(kind)
		{
		case LuaValue::Kind::Nil: return "nil";
		case LuaValue::Kind::Boolean: return "boolean";
		case LuaValue::Kind::Integer: return "integer";
		case LuaValue::Kind::Number: return "number";
		case LuaValue::Kind::String: return "string";
		case LuaValue::Kind::Identifier: return "identifier";
		case LuaValue::Kind::Object: return "object";
		}
		return "?";
	}

	namespace
	{
		struct Token
		{
			enum class Kind
			{
				End,
				Identifier,
				String,
				Number,
				LBrace,
				RBrace,
				LBracket,
				RBracket,
				Equals,
				Comma,
			};

			Kind kind = Kind::End;
			std::string text;
			bool isFloat = false;
			long long integer = 0;
			double number = 0.0;
			int line = 1;
			int column = 1;
		};

		struct Scanner
		{
			const std::string& source;
			size_t at = 0;
			int line = 1;
			int column = 1;

			explicit Scanner(const std::string& s) : source(s) {}

			void Advance()
			{
				if(source[at] == '\n')
				{
					line++;
					column = 1;
				}
				else
					column++;
				at++;
			}

			// `--` to end of line, and `//` the same, because both appear in real
			// files.  A lone `-` in front of digits is a negative number.
			void SkipSpaceAndComments()
			{
				while(at < source.size())
				{
					const char c = source[at];
					if(c == ' ' || c == '\t' || c == '\v' || c == '\n' || c == '\f' || c == '\r')
					{
						Advance();
						continue;
					}
					if(c == '-' && at + 1 < source.size() && source[at + 1] == '-')
					{
						while(at < source.size() && source[at] != '\n')
							Advance();
						continue;
					}
					if(c == '/' && at + 1 < source.size() && source[at + 1] == '/')
					{
						while(at < source.size() && source[at] != '\n')
							Advance();
						continue;
					}
					break;
				}
			}

			bool Next(Token& out, std::string& error)
			{
				SkipSpaceAndComments();
				out = Token{};
				out.line = line;
				out.column = column;
				if(at >= source.size())
				{
					out.kind = Token::Kind::End;
					return true;
				}
				const unsigned char c = (unsigned char)source[at];
				// [[ ... ]] long strings: real dialogue uses them whenever a line
				// contains a half-width quote, so they are not an optional extra
				if(c == '[' && at + 1 < source.size() && source[at + 1] == '[')
				{
					Advance();
					Advance();
					if(at < source.size() && source[at] == '\n')
						Advance(); // Lua drops a newline right after the opener
					std::string text;
					while(at + 1 < source.size()
						&& !(source[at] == ']' && source[at + 1] == ']'))
					{
						text.push_back(source[at]);
						Advance();
					}
					if(at + 1 >= source.size())
					{
						error = "unterminated long string at line " + std::to_string(line);
						return false;
					}
					Advance();
					Advance();
					out.kind = Token::Kind::String;
					out.text = text;
					return true;
				}
				switch(c)
				{
				case '{': out.kind = Token::Kind::LBrace; Advance(); return true;
				case '}': out.kind = Token::Kind::RBrace; Advance(); return true;
				case '[': out.kind = Token::Kind::LBracket; Advance(); return true;
				case ']': out.kind = Token::Kind::RBracket; Advance(); return true;
				case '=': out.kind = Token::Kind::Equals; Advance(); return true;
				case ',': out.kind = Token::Kind::Comma; Advance(); return true;
				default: break;
				}
				if(c == '"' || c == '\'')
				{
					const char quote = (char)c;
					Advance();
					std::string text;
					while(at < source.size() && source[at] != quote)
					{
						if(source[at] == '\n')
						{
							error = "unterminated string at line " + std::to_string(line);
							return false;
						}
						if(source[at] == '\\' && at + 1 < source.size())
						{
							const char next = source[at + 1];
							Advance();
							switch(next)
							{
							case 'n': text.push_back('\n'); break;
							case 't': text.push_back('\t'); break;
							case 'r': text.push_back('\r'); break;
							case '"': text.push_back('"'); break;
							case '\'': text.push_back('\''); break;
							case '\\': text.push_back('\\'); break;
							default: text.push_back(next); break;
							}
							Advance();
							continue;
						}
						// Shift-JIS and UTF-8 bytes pass through untouched
						text.push_back(source[at]);
						Advance();
					}
					if(at >= source.size())
					{
						error = "unterminated string at line " + std::to_string(line);
						return false;
					}
					Advance();
					out.kind = Token::Kind::String;
					out.text = text;
					return true;
				}
				if(c == '-' || (c >= '0' && c <= '9'))
				{
					std::string digits;
					if(c == '-')
					{
						digits.push_back('-');
						Advance();
					}
					bool seenDot = false;
					while(at < source.size())
					{
						const unsigned char d = (unsigned char)source[at];
						if(d >= '0' && d <= '9')
						{
							digits.push_back((char)d);
							Advance();
							continue;
						}
						if(d == '.' && !seenDot)
						{
							seenDot = true;
							digits.push_back('.');
							Advance();
							continue;
						}
						break;
					}
					out.kind = Token::Kind::Number;
					out.isFloat = seenDot;
					if(seenDot)
						out.number = strtod(digits.c_str(), nullptr);
					else
					{
						out.integer = strtoll(digits.c_str(), nullptr, 10);
						out.number = (double)out.integer;
					}
					return true;
				}
				if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'
					|| c >= 0x80)
				{
					std::string name;
					while(at < source.size())
					{
						const unsigned char d = (unsigned char)source[at];
						if((d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z')
							|| (d >= '0' && d <= '9') || d == '_' || d == '.'
							|| d == ':' || d >= 0x80)
						{
							name.push_back((char)d);
							Advance();
							continue;
						}
						break;
					}
					out.kind = Token::Kind::Identifier;
					out.text = name;
					return true;
				}
				error = std::string("unexpected character '") + (char)c + "' at line "
					+ std::to_string(line) + " column " + std::to_string(column);
				return false;
			}
		};

		struct Parser
		{
			Scanner scanner;
			Token token;
			std::string error;
			// A recursive-descent reader has one stack frame per nesting level, and
			// a hostile file can nest without limit: 128 KB of '[[' -free braces
			// was enough to overflow the stack and take the process down, which for
			// a compatibility layer means "the game will not open".  Real scripts
			// nest a handful deep, so a limit this generous costs nothing.
			static constexpr int kMaxDepth = 200;
			int depth = 0;

			explicit Parser(const std::string& text) : scanner(text) {}

			bool Advance() { return scanner.Next(token, error); }

			bool Expect(Token::Kind kind, const char* what)
			{
				if(token.kind != kind)
				{
					error = std::string("expected ") + what + " at line "
						+ std::to_string(token.line) + " column " + std::to_string(token.column);
					return false;
				}
				return true;
			}

			bool ParseObject(LuaValue& out)
			{
				out = LuaValue{};
				out.kind = LuaValue::Kind::Object;
				if(!Expect(Token::Kind::LBrace, "'{'"))
					return false;
				if(!Advance())
					return false;
				if(token.kind == Token::Kind::RBrace)
					return Advance();
				while(true)
				{
					// a member is either `key = value` or a bare value
					std::string key;
					bool hasKey = false;
					if(token.kind == Token::Kind::LBracket)
					{
						// ["瑠莉"] = "0,1,166,93"
						if(!Advance())
							return false;
						LuaValue keyValue;
						if(!ParseValue(keyValue))
							return false;
						if(!Expect(Token::Kind::RBracket, "']'"))
							return false;
						if(!Advance())
							return false;
						key = keyValue.kind == LuaValue::Kind::String ? keyValue.text
							: keyValue.summary();
						hasKey = true;
					}
					else if(token.kind == Token::Kind::Identifier)
					{
						key = token.text;
						const int line = token.line;
						if(!Advance())
							return false;
						if(token.kind == Token::Kind::Equals)
							hasKey = true;
						else
						{
							// a bare identifier value (`true`, `nil`, an enum name)
							LuaValue value;
							value.kind = LuaValue::Kind::Identifier;
							value.text = key;
							out.items.push_back(std::move(value));
							if(token.kind == Token::Kind::Comma)
							{
								if(!Advance())
									return false;
								if(token.kind == Token::Kind::RBrace)
									break;
								continue;
							}
							break;
						}
						if(hasKey && token.kind != Token::Kind::Equals)
						{
							error = "expected '=' after field at line " + std::to_string(line);
							return false;
						}
					}
					else if(token.kind == Token::Kind::String)
					{
						// a bare string is an array item; a string followed by '='
						// is a key too (Lua allows it)
						key = token.text;
						if(!Advance())
							return false;
						if(token.kind == Token::Kind::Equals)
							hasKey = true;
						else
						{
							LuaValue value;
							value.kind = LuaValue::Kind::String;
							value.text = key;
							out.items.push_back(std::move(value));
							if(token.kind == Token::Kind::Comma)
							{
								if(!Advance())
									return false;
								if(token.kind == Token::Kind::RBrace)
									break;
								continue;
							}
							break;
						}
					}
					if(hasKey)
					{
						if(!Expect(Token::Kind::Equals, "'='"))
							return false;
						if(!Advance())
							return false;
						LuaValue value;
						if(!ParseValue(value))
							return false;
						// keep the order; last write wins on lookup
						out.entries.emplace_back(key, std::move(value));
					}
					else
					{
						LuaValue value;
						if(!ParseValue(value))
							return false;
						out.items.push_back(std::move(value));
					}

					if(token.kind == Token::Kind::Comma)
					{
						if(!Advance())
							return false;
						if(token.kind == Token::Kind::RBrace)
							break; // trailing comma
						continue;
					}
					break;
				}
				if(!Expect(Token::Kind::RBrace, "'}'"))
					return false;
				return Advance();
			}

			bool ParseValue(LuaValue& out)
			{
				switch(token.kind)
				{
				case Token::Kind::LBrace:
					if(depth >= kMaxDepth)
					{
						error = "nesting deeper than " + std::to_string(kMaxDepth)
							+ " levels at line " + std::to_string(token.line);
						return false;
					}
					{
						++depth;
						const bool ok = ParseObject(out);
						--depth;
						return ok;
					}
				case Token::Kind::String:
					out = LuaValue{};
					out.kind = LuaValue::Kind::String;
					out.text = token.text;
					return Advance();
				case Token::Kind::Number:
					out = LuaValue{};
					out.kind = token.isFloat ? LuaValue::Kind::Number : LuaValue::Kind::Integer;
					out.integer = token.integer;
					out.number = token.number;
					out.asNumber = token.number;
					out.text = token.isFloat ? std::to_string(token.number)
						: std::to_string(token.integer);
					return Advance();
				case Token::Kind::Identifier:
					out = LuaValue{};
					if(token.text == "true" || token.text == "false")
					{
						out.kind = LuaValue::Kind::Boolean;
						out.boolean = token.text == "true";
					}
					else if(token.text == "nil")
						out.kind = LuaValue::Kind::Nil;
					else
					{
						out.kind = LuaValue::Kind::Identifier;
						out.text = token.text;
					}
					return Advance();
				default:
					error = "expected a value at line " + std::to_string(token.line);
					return false;
				}
			}

			bool ParseRoot(LuaValue& out, std::string* rootName)
			{
				// A real .ast opens with two assignments - `astver = 2.0` then
				// `ast = { ... }` - so the file is a sequence of them, and the
				// result is a table of those top-level names.
				out = LuaValue{};
				out.kind = LuaValue::Kind::Object;
				if(!Advance())
					return false;
				if(token.kind == Token::Kind::End)
				{
					error = "the file has no assignments";
					return false;
				}
				bool first = true;
				while(token.kind != Token::Kind::End)
				{
					if(token.kind != Token::Kind::Identifier)
					{
						error = "expected a name at line " + std::to_string(token.line)
							+ " column " + std::to_string(token.column);
						return false;
					}
					const std::string name = token.text;
					if(first && rootName)
						*rootName = name;
					first = false;
					if(!Advance() || !Expect(Token::Kind::Equals, "'='"))
						return false;
					if(!Advance())
						return false;
					LuaValue value;
					if(!ParseValue(value))
						return false;
					out.entries.emplace_back(name, std::move(value));
					if(token.kind == Token::Kind::Comma)
					{
						if(!Advance())
							return false;
					}
				}
				return true;
			}
		};
	}

	bool ParseLuaTable(const std::string& text, LuaValue& root, std::string& error,
		std::string* rootName)
	{
		Parser parser(text);
		if(!parser.ParseRoot(root, rootName))
		{
			error = parser.error;
			return false;
		}
		return true;
	}
}