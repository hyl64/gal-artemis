// json.cpp - the reader/writer described in json.h

#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace artemis
{
	namespace save
	{
		const Json* Json::field(const std::string& name) const
		{
			const Json* found = nullptr;
			for(const auto& f : fields)
				if(f.first == name)
					found = &f.second;
			return found;
		}

		std::string Json::stringOr(const std::string& name, const std::string& fallback) const
		{
			const Json* value = field(name);
			if(value == nullptr)
				return fallback;
			if(value->kind == Kind::String)
				return value->text;
			if(value->kind == Kind::Number)
				return value->text;
			return fallback;
		}

		double Json::numberOr(const std::string& name, double fallback) const
		{
			const Json* value = field(name);
			if(value != nullptr && value->kind == Kind::Number)
				return value->number;
			if(value != nullptr && value->kind == Kind::String) // "3" is a slot
				return strtod(value->text.c_str(), nullptr);
			return fallback;
		}

		bool Json::boolOr(const std::string& name, bool fallback) const
		{
			const Json* value = field(name);
			if(value != nullptr && value->kind == Kind::Bool)
				return value->boolean;
			return fallback;
		}

		Json Json::MakeString(const std::string& value)
		{
			Json out;
			out.kind = Kind::String;
			out.text = value;
			return out;
		}

		Json Json::MakeNumber(double value)
		{
			Json out;
			out.kind = Kind::Number;
			out.number = value;
			if(value == (double)(long long)value && fabs(value) < 1e15)
			{
				char buffer[32];
				snprintf(buffer, sizeof(buffer), "%lld", (long long)value);
				out.text = buffer;
			}
			else
			{
				char buffer[64];
				snprintf(buffer, sizeof(buffer), "%.10g", value);
				out.text = buffer;
			}
			return out;
		}

		Json Json::MakeBool(bool value)
		{
			Json out;
			out.kind = Kind::Bool;
			out.boolean = value;
			return out;
		}

		Json Json::MakeObject()
		{
			Json out;
			out.kind = Kind::Object;
			return out;
		}

		Json Json::MakeArray()
		{
			Json out;
			out.kind = Kind::Array;
			return out;
		}

		void Json::Set(const std::string& name, Json value)
		{
			kind = Kind::Object;
			for(auto& f : fields)
				if(f.first == name)
				{
					f.second = std::move(value);
					return;
				}
			fields.emplace_back(name, std::move(value));
		}

		void Json::Push(Json value)
		{
			kind = Kind::Array;
			items.push_back(std::move(value));
		}

		namespace
		{
			std::string Escape(const std::string& in)
			{
				std::string out;
				out.reserve(in.size() + 8);
				for(unsigned char c : in)
				{
					switch(c)
					{
					case '"': out += "\\\""; break;
					case '\\': out += "\\\\"; break;
					case '\n': out += "\\n"; break;
					case '\r': out += "\\r"; break;
					case '\t': out += "\\t"; break;
					default:
						if(c < 0x20)
						{
							char buffer[8];
							snprintf(buffer, sizeof(buffer), "\\u%04x", c);
							out += buffer;
						}
						else
							out.push_back((char)c); // UTF-8 passes through
					}
				}
				return out;
			}

			void DumpInto(const Json& value, std::string& out, int indent, int depth)
			{
				const std::string pad = (indent > 0) ? std::string((size_t)(indent * depth), ' ') : std::string();
				const std::string padInner = (indent > 0)
					? std::string((size_t)(indent * (depth + 1)), ' ') : std::string();
				switch(value.kind)
				{
				case Json::Kind::Null: out += "null"; break;
				case Json::Kind::Bool: out += value.boolean ? "true" : "false"; break;
				case Json::Kind::Number: out += value.text.empty() ? std::to_string(value.number) : value.text; break;
				case Json::Kind::String: out += "\"" + Escape(value.text) + "\""; break;
				case Json::Kind::Array:
					if(value.items.empty())
					{
						out += "[]";
						break;
					}
					out += "[";
					for(size_t i = 0; i < value.items.size(); i++)
					{
						if(i)
							out += ",";
						if(indent > 0)
							out += "\n" + padInner;
						DumpInto(value.items[i], out, indent, depth + 1);
					}
					if(indent > 0)
						out += "\n" + pad;
					out += "]";
					break;
				case Json::Kind::Object:
					if(value.fields.empty())
					{
						out += "{}";
						break;
					}
					out += "{";
					for(size_t i = 0; i < value.fields.size(); i++)
					{
						if(i)
							out += ",";
						if(indent > 0)
							out += "\n" + padInner;
						out += "\"" + Escape(value.fields[i].first) + "\":";
						if(indent > 0)
							out += " ";
						DumpInto(value.fields[i].second, out, indent, depth + 1);
					}
					if(indent > 0)
						out += "\n" + pad;
					out += "}";
					break;
				}
			}
		}

		std::string Json::Dump(int indent) const
		{
			std::string out;
			DumpInto(*this, out, indent, 0);
			return out;
		}

		// ------------------------------------------------------------- parsing

		namespace
		{
			struct Reader
			{
				const std::string& text;
				size_t at = 0;
				int line = 1;
				std::string error;
				int depth = 0;

				explicit Reader(const std::string& s) : text(s) {}

				bool Fail(const std::string& why)
				{
					error = why + " at line " + std::to_string(line);
					return false;
				}

				void Skip()
				{
					while(at < text.size())
					{
						const char c = text[at];
						if(c == ' ' || c == '\t' || c == '\r')
							at++;
						else if(c == '\n')
						{
							line++;
							at++;
						}
						else
							break;
					}
				}

				bool ParseValue(Json& out)
				{
					if(++depth > 200)
						return Fail("JSON nested too deeply");
					Skip();
					if(at >= text.size())
						return Fail("unexpected end of input");
					const char c = text[at];
					if(c == '{')
					{
						at++;
						out = Json::MakeObject();
						Skip();
						if(at < text.size() && text[at] == '}')
						{
							at++;
							depth--;
							return true;
						}
						while(true)
						{
							Skip();
							if(at >= text.size() || text[at] != '"')
								return Fail("expected an object key");
							std::string key;
							if(!ParseString(key))
								return false;
							Skip();
							if(at >= text.size() || text[at] != ':')
								return Fail("expected ':' after an object key");
							at++;
							Json value;
							if(!ParseValue(value))
								return false;
							out.Set(key, std::move(value));
							Skip();
							if(at < text.size() && text[at] == ',')
							{
								at++;
								continue;
							}
							if(at < text.size() && text[at] == '}')
							{
								at++;
								depth--;
								return true;
							}
							return Fail("expected ',' or '}' in an object");
						}
					}
					if(c == '[')
					{
						at++;
						out = Json::MakeArray();
						Skip();
						if(at < text.size() && text[at] == ']')
						{
							at++;
							depth--;
							return true;
						}
						while(true)
						{
							Json value;
							if(!ParseValue(value))
								return false;
							out.Push(std::move(value));
							Skip();
							if(at < text.size() && text[at] == ',')
							{
								at++;
								continue;
							}
							if(at < text.size() && text[at] == ']')
							{
								at++;
								depth--;
								return true;
							}
							return Fail("expected ',' or ']' in an array");
						}
					}
					if(c == '"')
					{
						std::string value;
						if(!ParseString(value))
							return false;
						out = Json::MakeString(value);
						depth--;
						return true;
					}
					if(text.compare(at, 4, "true") == 0)
					{
						at += 4;
						out = Json::MakeBool(true);
						depth--;
						return true;
					}
					if(text.compare(at, 5, "false") == 0)
					{
						at += 5;
						out = Json::MakeBool(false);
						depth--;
						return true;
					}
					if(text.compare(at, 4, "null") == 0)
					{
						at += 4;
						out = Json{};
						depth--;
						return true;
					}
					// a number
					const size_t start = at;
					if(at < text.size() && (text[at] == '-' || text[at] == '+'))
						at++;
					while(at < text.size() && ((text[at] >= '0' && text[at] <= '9')
						|| text[at] == '.' || text[at] == 'e' || text[at] == 'E'
						|| text[at] == '-' || text[at] == '+'))
						at++;
					if(at == start)
						return Fail(std::string("unexpected character '") + c + "'");
					out = Json::MakeNumber(strtod(text.substr(start, at - start).c_str(), nullptr));
					out.text = text.substr(start, at - start);
					depth--;
					return true;
				}

				bool ParseString(std::string& out)
				{
					at++; // the opening quote
					out.clear();
					while(at < text.size() && text[at] != '"')
					{
						const char c = text[at];
						if(c == '\\')
						{
							at++;
							if(at >= text.size())
								return Fail("unterminated escape");
							switch(text[at])
							{
							case 'n': out.push_back('\n'); break;
							case 't': out.push_back('\t'); break;
							case 'r': out.push_back('\r'); break;
							case 'b': out.push_back('\b'); break;
							case 'f': out.push_back('\f'); break;
							case '"': out.push_back('"'); break;
							case '\\': out.push_back('\\'); break;
							case '/': out.push_back('/'); break;
							case 'u':
							{
								if(at + 4 >= text.size())
									return Fail("truncated \\u escape");
								const std::string hex = text.substr(at + 1, 4);
								const unsigned code = (unsigned)strtoul(hex.c_str(), nullptr, 16);
								// BMP only; surrogate pairs are not worth it here
								if(code < 0x80)
									out.push_back((char)code);
								else if(code < 0x800)
								{
									out.push_back((char)(0xC0 | (code >> 6)));
									out.push_back((char)(0x80 | (code & 0x3F)));
								}
								else
								{
									out.push_back((char)(0xE0 | (code >> 12)));
									out.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
									out.push_back((char)(0x80 | (code & 0x3F)));
								}
								at += 4;
								break;
							}
							default: out.push_back(text[at]); break;
							}
							at++;
							continue;
						}
						if(c == '\n')
						{
							line++;
							return Fail("a newline inside a string");
						}
						out.push_back(c);
						at++;
					}
					if(at >= text.size())
						return Fail("unterminated string");
					at++;
					return true;
				}
			};
		}

		bool ParseJson(const std::string& text, Json& out, std::string& error)
		{
			Reader reader(text);
			if(!reader.ParseValue(out))
			{
				error = reader.error;
				return false;
			}
			reader.Skip();
			if(reader.at != text.size())
			{
				error = "trailing text after the JSON value at line "
					+ std::to_string(reader.line);
				return false;
			}
			return true;
		}
	}
}