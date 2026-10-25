// luaast.cpp - pull blocks, commands and dialogue out of a real .ast

#include "luaast.h"
#include "lua.h"

#include <algorithm>
#include <map>

namespace artemis
{
	static std::string ValueText(const LuaValue& v, bool* quoted = nullptr)
	{
		switch(v.kind)
		{
		case LuaValue::Kind::String:
			if(quoted)
				*quoted = true;
			return v.text;
		case LuaValue::Kind::Integer:
			return std::to_string(v.integer);
		case LuaValue::Kind::Number:
		{
			// keep the file's own spelling where it matters: 2.0 is a version,
			// 2 would be a different string
			std::string text = std::to_string(v.number);
			while(text.size() > 1 && text.back() == '0' && text[text.size() - 2] != '.')
				text.pop_back();
			if(!text.empty() && text.back() == '.')
				text.push_back('0');
			return text;
		}
		case LuaValue::Kind::Boolean:
			return v.boolean ? "true" : "false";
		case LuaValue::Kind::Identifier:
			return v.text;
		case LuaValue::Kind::Nil:
			return "nil";
		case LuaValue::Kind::Object:
			return v.summary();
		}
		return "";
	}

	// A command is an object whose first array item is a string; that string is
	// the command name and the named fields are its parameters.
	static bool AsCommand(const LuaValue& v, AstCommand& out)
	{
		if(!v.isObject() || v.items.empty())
			return false;
		if(v.items[0].kind != LuaValue::Kind::String)
			return false;
		out.name = v.items[0].text;
		for(const auto& e : v.entries)
		{
			AstAttribute attribute;
			attribute.name = e.first;
			attribute.value = ValueText(e.second, &attribute.quoted);
			out.attributes.push_back(std::move(attribute));
		}
		// some commands carry extra array items; say so rather than drop them
		for(size_t i = 1; i < v.items.size(); i++)
		{
			AstAttribute attribute;
			attribute.name = "";
			attribute.value = ValueText(v.items[i]);
			out.attributes.push_back(std::move(attribute));
		}
		return true;
	}

	// One entry of a `text` group: `{ name = {...}, "line", {"rt2"} }`.  The
	// speaker is a named field of the entry, the line is the first bare string.
	static void ReadLineEntry(const LuaValue& entry, AstLine& line)
	{
		for(const auto& item : entry.entries)
		{
			if(item.first != "name")
				continue;
			std::string joined;
			for(const LuaValue& part : item.second.items)
			{
				if(part.kind != LuaValue::Kind::String)
					continue;
				if(!joined.empty())
					joined += ",";
				joined += part.text;
			}
			if(joined.empty() && item.second.kind == LuaValue::Kind::String)
				joined = item.second.text;
			if(!joined.empty())
				line.speaker = joined;
		}
		for(const LuaValue& item : entry.items)
		{
			if(item.kind == LuaValue::Kind::String)
			{
				// a line can be several strings in a row: `[[「先日...、"ア]]`,
				// `{"ruby", text="べん"}`, `"便"` is one line of dialogue
				line.text += item.text;
				continue;
			}
			if(item.isObject())
			{
				// an inline ruby or similar carries its own reading in `text`
				if(const LuaValue* ruby = item.field("text"))
					if(ruby->kind == LuaValue::Kind::String)
						line.text += ruby->text;
				if(const LuaValue* name = item.field("name"))
				{
					std::string joined;
					for(const LuaValue& part : name->items)
					{
						if(part.kind != LuaValue::Kind::String)
							continue;
						if(!joined.empty())
							joined += ",";
						joined += part.text;
					}
					if(joined.empty())
						joined = ValueText(*name);
					if(!joined.empty())
						line.speaker = joined;
				}
				else
				{
					// a marker like {"rt2"}: keep its name as a tag
					for(const LuaValue& part : item.items)
						if(part.kind == LuaValue::Kind::String)
							line.tags.push_back(part.text);
				}
			}
		}
	}

	static void ReadTextGroup(const LuaValue& group, std::vector<AstLine>& lines,
		std::vector<std::string>& voices, bool voiceGroup)
	{
		if(group.kind == LuaValue::Kind::String)
		{
			AstLine line;
			line.text = group.text;
			lines.push_back(std::move(line));
			return;
		}
		if(!group.isObject())
			return;
		for(const LuaValue& entry : group.items)
		{
			if(voiceGroup)
			{
				// vo entries are commands: {"vo", file="...", ch="..."}
				std::string file;
				if(entry.isObject())
					if(const LuaValue* f = entry.field("file"))
						file = ValueText(*f);
				if(!file.empty())
					voices.push_back(file);
				continue;
			}
			if(entry.kind == LuaValue::Kind::String)
			{
				AstLine line;
				line.text = entry.text;
				lines.push_back(std::move(line));
				continue;
			}
			if(entry.isObject())
			{
				AstLine line;
				ReadLineEntry(entry, line);
				if(!line.text.empty())
					lines.push_back(std::move(line));
			}
		}
		// a group can also carry named entries; take them in file order
		for(const auto& e : group.entries)
		{
			if(e.second.kind == LuaValue::Kind::String)
			{
				AstLine line;
				line.text = e.second.text;
				lines.push_back(std::move(line));
			}
		}
	}

	static void ReadBlock(const LuaValue& value, const std::string& name, AstBlock& out)
	{
		out.name = name;
		if(!value.isObject())
			return;
		for(const LuaValue& item : value.items)
		{
			AstCommand command;
			if(AsCommand(item, command))
				out.commands.push_back(std::move(command));
		}
		// named fields that hold commands (rare, but the dialect allows it)
		for(const auto& e : value.entries)
		{
			AstCommand command;
			if(AsCommand(e.second, command))
				out.commands.push_back(std::move(command));
		}
		if(const LuaValue* text = value.field("text"))
		{
			if(const LuaValue* ja = text->field("ja"))
				ReadTextGroup(*ja, out.lines, out.voices, false);
			if(const LuaValue* vo = text->field("vo"))
			{
				std::vector<AstLine> notLines; // the voice group yields file names
				ReadTextGroup(*vo, notLines, out.voices, true);
			}
			// A third shape the arrayed layout uses: `text` is a dictionary keyed
			// by the message index, and each value carries its own `ja` group
			// (`text = { [8] = { name={name="悟"}, ja={ {"「…」"} } } }`).  Reading
			// only the array shape is why this layout reported zero lines.
			bool indexed = false;
			for(const auto& entry : text->entries)
			{
				if(!entry.second.field("ja"))
					continue;
				indexed = true;
				if(const LuaValue* vo = entry.second.field("vo"))
				{
					std::vector<AstLine> notLines;
					ReadTextGroup(*vo, notLines, out.voices, true);
				}
				ReadTextGroup(*entry.second.field("ja"), out.lines, out.voices, false);
			}
			if(!indexed && !text->field("ja") && !text->field("vo"))
				ReadTextGroup(*text, out.lines, out.voices, false);
		}
		if(const LuaValue* next = value.field("linknext"))
			out.next = ValueText(*next);
		if(const LuaValue* lang = value.field("lang"))
			out.lang = ValueText(*lang);
		if(const LuaValue* line = value.field("line"))
			if(line->kind == LuaValue::Kind::Integer || line->kind == LuaValue::Kind::Number)
				out.line = (int)line->asNumber;
	}

	bool ParseLuaAst(const std::string& text, LuaAst& out, std::string& error)
	{
		LuaValue root;
		std::string firstKey;
		if(!ParseLuaTable(text, root, error, &firstKey))
			return false;
		if(const LuaValue* version = root.field("astver"))
			out.version = ValueText(*version);
		const LuaValue* ast = root.field("ast");
		if(ast == nullptr)
		{
			error = "no 'ast' table (the file opens with '" + firstKey + "')";
			return false;
		}
		if(!ast->isObject())
		{
			error = "the 'ast' table is not an object";
			return false;
		}

		// B variant: the top level is an array of blocks whose dialogue lives in
		// a separate lang_NNNNN file; A variant: named block_NNNNN tables
		const bool arrayed = !ast->items.empty()
			&& ast->items[0].isObject();
		out.arrayed = arrayed;
		if(arrayed)
		{
			for(size_t i = 0; i < ast->items.size(); i++)
			{
				AstBlock block;
				ReadBlock(ast->items[i], "", block);
				out.blocks.push_back(std::move(block));
			}
			// The dialogue itself sits beside the block array, in `ast.text`, keyed
			// by message number: `[8] = { name={...}, ja={ {"「…」"} }, vo={...} }`.
			// A block that carries a {"text"} command consumes the next message
			// number, in block order, which is how the two tables line up.
			const LuaValue* messages = ast->field("text");
			size_t external = 0;
			if(messages != nullptr && !messages->entries.empty())
			{
				std::map<long long, const LuaValue*> byIndex;
				long long highest = 0;
				for(const auto& e : messages->entries)
				{
					// A bracketed key `[8] = {...}` is recorded as "integer 8".
					const std::string k = e.first.rfind("integer ", 0) == 0
						? e.first.substr(8) : e.first;
					char* end = nullptr;
					const long long n = std::strtoll(k.c_str(), &end, 10);
					if(end == nullptr || *end != '\0')
						continue;
					byIndex[n] = &e.second;
					if(n > highest)
						highest = n;
				}
				// The mapping is per block index, not "consume one message per
				// {'text'} command": block #i owns message i+1.  A block whose
				// script omits {'text'} can still own a message, so counting
				// commands would lose those lines.
				for(size_t i = 0; i < out.blocks.size(); i++)
				{
					const auto it = byIndex.find((long long)i + 1);
					if(it == byIndex.end())
						continue;
					AstBlock& b = out.blocks[i];
					if(const LuaValue* ja = it->second->field("ja"))
						ReadTextGroup(*ja, b.lines, b.voices, false);
					if(const LuaValue* vo = it->second->field("vo"))
					{
						std::vector<AstLine> notLines;
						ReadTextGroup(*vo, notLines, b.voices, true);
					}
				}
			}
			for(const AstBlock& b : out.blocks)
				if(!b.lang.empty() && b.lines.empty())
					external++;
			if(external > 0)
				out.notes.push_back(std::to_string(external) + " block(s) keep their "
					"dialogue in an external lang_NNNNN file");
		}
		else
		{
			// follow the linknext chain from the first block when there is one,
			// because that is the order the script reads
			std::string order;
			for(const auto& e : ast->entries)
			{
				if(e.first.rfind("block_", 0) == 0)
				{
					order = e.first;
					break;
				}
			}
			std::vector<std::string> seen;
			std::string at = order;
			while(!at.empty())
			{
				const LuaValue* value = ast->field(at);
				if(value == nullptr)
					break;
				if(std::find(seen.begin(), seen.end(), at) != seen.end())
				{
					out.notes.push_back("the linknext chain loops at " + at);
					break;
				}
				seen.push_back(at);
				AstBlock block;
				ReadBlock(*value, at, block);
				out.blocks.push_back(std::move(block));
				at = block.next;
			}
			// any block the chain did not reach is appended, in file order
			for(const auto& e : ast->entries)
			{
				if(e.first.rfind("block_", 0) != 0)
					continue;
				if(std::find(seen.begin(), seen.end(), e.first) != seen.end())
					continue;
				AstBlock block;
				ReadBlock(e.second, e.first, block);
				out.blocks.push_back(std::move(block));
			}
			if(ast->entries.empty())
				out.notes.push_back("the 'ast' table holds no blocks");
		}
		if(out.blocks.empty())
		{
			error = "the 'ast' table has no script blocks";
			return false;
		}
		return true;
	}

	std::vector<SpokenLine> SpokenLines(const LuaAst& ast)
	{
		std::vector<SpokenLine> out;
		for(const AstBlock& block : ast.blocks)
			for(const AstLine& line : block.lines)
			{
				SpokenLine spoken;
				spoken.block = block.name;
				spoken.speaker = line.speaker;
				spoken.text = line.text;
				out.push_back(std::move(spoken));
			}
		return out;
	}
}