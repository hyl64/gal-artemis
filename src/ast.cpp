// ast.cpp - tokeniser and parser for the Artemis script text

#include "ast.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace artemis
{
	namespace
	{
		struct Token
		{
			enum Kind { Equal, OpenBrace, CloseBrace, Comma, Identifier, String, Integer, Float, Tag }
				kind;
			std::string text;
			int64_t integer = 0;
			size_t at = 0;
		};

		void Fail(size_t at, const std::string& what)
		{
			char buf[256];
			snprintf(buf, sizeof buf, "ast: %s at byte %zu", what.c_str(), at);
			throw std::runtime_error(buf);
		}

		std::vector<Token> Lex(const std::string& s)
		{
			std::vector<Token> out;
			size_t i = 0;
			while(i < s.size())
			{
				const char c = s[i];
				if(c == ' ' || c == '\t' || c == '\r' || c == '\n')
				{
					i++;
					continue;
				}
				Token t;
				t.at = i;
				if(c == '=')
				{
					t.kind = Token::Equal;
					i++;
				}
				else if(c == '{')
				{
					t.kind = Token::OpenBrace;
					i++;
				}
				else if(c == '}')
				{
					t.kind = Token::CloseBrace;
					i++;
				}
				else if(c == ',')
				{
					t.kind = Token::Comma;
					i++;
				}
				else if(c == '"')
				{
					t.kind = Token::String;
					i++;
					while(i < s.size() && s[i] != '"')
					{
						// the format keeps backslash escapes as written
						if(s[i] == '\\' && i + 1 < s.size())
						{
							t.text.push_back(s[i]);
							i++;
						}
						t.text.push_back(s[i]);
						i++;
					}
					if(i >= s.size())
						Fail(t.at, "string runs to the end of the file");
					i++; // closing quote
				}
				else if(c == '[')
				{
					// a tag: [ ] or [123]
					t.kind = Token::Tag;
					i++;
					std::string digits;
					while(i < s.size() && s[i] != ']')
					{
						if(isdigit((unsigned char)s[i]))
							digits.push_back(s[i]);
						i++;
					}
					if(i < s.size())
						i++; // closing bracket
					if(!digits.empty())
					{
						t.integer = atoll(digits.c_str());
						t.text = digits;
					}
				}
				else if(isdigit((unsigned char)c) || (c == '-' && i + 1 < s.size()
					&& isdigit((unsigned char)s[i + 1])))
				{
					bool isFloat = false;
					size_t start = i;
					if(s[i] == '-')
						i++;
					while(i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == '.'))
					{
						if(s[i] == '.')
							isFloat = true;
						i++;
					}
					t.text = s.substr(start, i - start);
					t.kind = isFloat ? Token::Float : Token::Integer;
					if(!isFloat)
						t.integer = atoll(t.text.c_str());
				}
				else
				{
					size_t start = i;
					while(i < s.size() && (isalnum((unsigned char)s[i]) || s[i] == '_'))
						i++;
					if(start == i)
						Fail(i, "unexpected character");
					t.kind = Token::Identifier;
					t.text = s.substr(start, i - start);
				}
				out.push_back(t);
			}
			return out;
		}

		struct Parser
		{
			const std::vector<Token>& tokens;
			size_t at = 0;

			explicit Parser(const std::vector<Token>& t) : tokens(t) {}

			const Token& Peek() const
			{
				if(at >= tokens.size())
					Fail(tokens.empty() ? 0 : tokens.back().at, "the file ends early");
				return tokens[at];
			}

			Node Value()
			{
				const Token& t = Peek();
				switch(t.kind)
				{
				case Token::OpenBrace:
				{
					at++;
					Node node;
					node.type = NodeType::Array;
					while(Peek().kind != Token::CloseBrace)
					{
						// `name = value` and `"quoted name" = value` are members of
						// this block, not elements of a list
						const Token& head = Peek();
						if((head.kind == Token::Identifier || head.kind == Token::String)
							&& at + 1 < tokens.size() && tokens[at + 1].kind == Token::Equal)
						{
							if(node.type == NodeType::Array && !node.items.empty()
								&& node.members.empty())
							{
								// the block turned out to be a dictionary: keep what
								// came before as keyless members
								for(auto& item : node.items)
									node.members.emplace_back(std::string(), item);
								node.items.clear();
							}
							node.type = NodeType::Dictionary;
							at += 2; // name and =
							node.members.emplace_back(head.text, Value());
						}
						else
							node.items.push_back(Value());
						if(Peek().kind == Token::Comma)
							at++;
					}
					at++; // }
					return node;
				}
				case Token::String:
				{
					at++;
					Node node;
					node.type = NodeType::String;
					node.text = t.text;
					return node;
				}
				case Token::Integer:
				{
					at++;
					Node node;
					node.type = NodeType::Integer;
					node.integer = t.integer;
					node.text = t.text;
					return node;
				}
				case Token::Float:
				{
					at++;
					Node node;
					node.type = NodeType::Float;
					node.text = t.text;
					return node;
				}
				case Token::Tag:
				{
					at++;
					Node node;
					node.type = NodeType::Tag;
					node.integer = t.integer;
					node.hasTagNumber = !t.text.empty();
					node.text = t.text;
					return node;
				}
				case Token::Identifier:
				{
					at++;
					// `name = value` inside a value position is a one-member
					// dictionary, which is how the format nests blocks
					if(Peek().kind == Token::Equal)
					{
						at++;
						Node node;
						node.type = NodeType::Dictionary;
						node.members.emplace_back(t.text, Value());
						return node;
					}
					Node node;
					node.type = NodeType::Identifier;
					node.text = t.text;
					return node;
				}
				default:
					Fail(t.at, "unexpected token");
				}
				return Node();
			}

			Script File()
			{
				Script script;
				while(at < tokens.size())
				{
					const Token& name = Peek();
					if(name.kind != Token::Identifier)
						Fail(name.at, "expected a name at the top level");
					at++;
					if(Peek().kind != Token::Equal)
						Fail(Peek().at, "expected = after a name");
					at++;
					script.top.emplace_back(name.text, Value());
				}
				return script;
			}
		};

		void DumpValue(const Node& node, std::string& out, int depth);

		void Indent(std::string& out, int depth)
		{
			out.append((size_t)depth, '\t');
		}

		void DumpMembers(const std::vector<std::pair<std::string, Node>>& members, std::string& out,
			int depth)
		{
			for(auto& m : members)
			{
				Indent(out, depth);
				out += m.first;
				out += " = ";
				DumpValue(m.second, out, depth);
				out += "\n";
			}
		}

		void DumpValue(const Node& node, std::string& out, int depth)
		{
			switch(node.type)
			{
			case NodeType::String:
				out += "\"";
				out += node.text;
				out += "\"";
				break;
			case NodeType::Integer:
			case NodeType::Float:
				out += node.text;
				break;
			case NodeType::Identifier:
				out += node.text;
				break;
			case NodeType::Tag:
				out += "[";
				if(node.hasTagNumber)
					out += node.text;
				out += "]";
				break;
			case NodeType::Dictionary:
			{
				// always with braces: the parser accepts the flatter
				// `name = value` spelling too, but a dump that keeps the
				// braces reads as the nesting it is
				out += "{\n";
				int depth2 = depth + 1;
				for(auto& m : node.members)
				{
					Indent(out, depth2);
					if(m.first.empty())
					{
						DumpValue(m.second, out, depth2);
						out += "\n";
						continue;
					}
					// a key that is not a plain name is written quoted
					bool plain = !m.first.empty();
					for(char c : m.first)
						if(!isalnum((unsigned char)c) && c != '_')
							plain = false;
					if(plain)
						out += m.first;
					else
					{
						out += "\"";
						out += m.first;
						out += "\"";
					}
					out += " = ";
					DumpValue(m.second, out, depth2);
					out += "\n";
				}
				Indent(out, depth);
				out += "}";
				break;
			}
			case NodeType::Array:
				out += "{\n";
				for(auto& item : node.items)
				{
					Indent(out, depth + 1);
					DumpValue(item, out, depth + 1);
					out += "\n";
				}
				Indent(out, depth);
				out += "}";
				break;
			}
		}
	}

	Script Parse(const std::string& text)
	{
		std::vector<Token> tokens = Lex(text);
		Parser parser(tokens);
		return parser.File();
	}

	std::string Dump(const Script& script)
	{
		std::string out;
		DumpMembers(script.top, out, 0);
		return out;
	}
}
