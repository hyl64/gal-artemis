// vm.cpp - running a real Artemis script
//
// The interesting parts, in the order they mattered while this was written
// against a real title:
//
//  * [rp] is the page break, and it is an *engine* command (the real scene uses
//    it 409 times).  A page with no text does not wait for a click, which is why
//    the macro files can use [rp] to clear a message layer.
//  * `[girl]`, `[bg_b]`, `[adv]`, `[n]` are macros, not commands.  They are
//    `*girl`, `*bg_b`, `*adv`, `*n` blocks in the game's own macro files, and
//    the only way to know that is to run macroadd and resolve the name against
//    the macro table before declaring it unknown.
//  * macro.iet itself is not in the game's macroadd list - the line is commented
//    out in the game's Lua.  It is the engine's global macro library (the real
//    file lives at the archive root, and the commented-out line is exactly the
//    same macroadd), so boot adds it first; without it `[adv]` has no definition
//    and the first screen has no message window.
//  * the game's own Lua is not skipped blindly: [calllua] takes the `e:tag{...}`
//    calls out of the named function and runs them.  That is where the screen
//    size and the macroadd list actually come from.

#include "vm.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace artemis
{
	namespace
	{
		std::string Trim(const std::string& text)
		{
			size_t start = 0, end = text.size();
			while(start < end && (text[start] == ' ' || text[start] == '\t' ||
				text[start] == '\r' || text[start] == '\n'))
				start++;
			while(end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
				text[end - 1] == '\r' || text[end - 1] == '\n'))
				end--;
			return text.substr(start, end - start);
		}

		std::string NormalizePath(std::string name)
		{
			for(char& c : name)
				if(c == '\\')
					c = '/';
			while(name.rfind("./", 0) == 0)
				name = name.substr(2);
			while(!name.empty() && name.front() == '/')
				name.erase(name.begin());
			const std::string suffix = ".iet";
			if(name.size() > suffix.size() &&
				name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
				name = name.substr(0, name.size() - suffix.size());
			for(char& c : name)
				c = (char)std::tolower((unsigned char)c);
			return name;
		}

		// Lua blocks close with `end`, not with a brace: `function f(e) ... end`.
		// The first version matched braces and therefore took the body of the
		// first e:tag{...} call as the function body, so the boot ran zero of the
		// game's own tag calls and none of its macro files were registered.
		size_t LuaBlockEnd(const std::string& source, size_t start)
		{
			int depth = 1;
			size_t at = start;
			while(at < source.size())
			{
				const char c = source[at];
				if(c == '"' || c == '\'')
				{
					const char quote = c;
					at++;
					while(at < source.size() && source[at] != quote)
					{
						if(source[at] == '\\')
							at++;
						at++;
					}
					at++;
					continue;
				}
				if(std::isalpha((unsigned char)c) != 0 || c == '_')
				{
					const size_t from = at;
					while(at < source.size() && (std::isalnum((unsigned char)source[at]) != 0 ||
						source[at] == '_'))
						at++;
					const std::string word = source.substr(from, at - from);
					// `do` is part of `for`/`while`, so it does not open a block of
					// its own
					if(word == "function" || word == "if" || word == "for" || word == "while")
						depth++;
					else if(word == "end")
					{
						depth--;
						if(depth == 0)
							return at;
					}
					continue;
				}
				at++;
			}
			return std::string::npos;
		}

		size_t MatchBrace(const std::string& source, size_t open)
		{
			int depth = 0;
			for(size_t i = open; i < source.size(); i++)
			{
				const char c = source[i];
				if(c == '"' || c == '\'')
				{
					const char quote = c;
					i++;
					while(i < source.size() && source[i] != quote)
					{
						if(source[i] == '\\')
							i++;
						i++;
					}
					continue;
				}
				if(c == '{')
					depth++;
				else if(c == '}')
				{
					depth--;
					if(depth == 0)
						return i;
				}
			}
			return std::string::npos;
		}

		// The tags a front end takes over when it attaches a sink.  Sound needs a
		// device; the rest need a frame time.  This list is deliberately a subset of
		// IsKnownIgnored() below: everything here is still counted and located when
		// no sink is attached.
		bool IsFrontEndTag(const std::string& name)
		{
			static const char* list[] = {
				// sound: the engine tags *and* the ones the GUI macros are built from
				// (`[bgm]` is `*bgm`, which calls `[splay]`; both are real names here)
				"bgm", "splay", "sxfade", "seplay", "playse", "se", "sstop", "stopse",
				"sestop", "bgmstop", "voice", "vstop", "mbgm", "fade", "fadein", "fadeout",
				// effects that move with the clock
				"sfade", "quake", "anime", "lytween", "lytweendel", "scetween", "flash",
				// input wiring and choices
				"setonpush", "select", "waitclick",
			};
			for(const char* entry : list)
				if(name == entry)
					return true;
			return false;
		}

		// the tags a real game uses that this tree knows about but does not act
		// on yet: audio, animation, UI wiring and engine bookkeeping.  They are
		// counted and reported, not silently dropped - see ignoredTags().
		bool IsKnownIgnored(const std::string& name)
		{
			static const char* list[] = {
				// audio
				"bgm", "seplay", "playse", "sstop", "stopse", "se", "bgmstop", "fade",
				"fadeout", "sfade", "fadein",
				// animation and screen effects
				"quake", "black", "white", "noise", "noisestop", "anime", "lytween",
				"lytweendel", "scetween", "glyph", "flash", "mask", "mask_del", "cinesco",
				"cinesco_del", "frame_cg", "lyedit", "lymove", "lymask", "lyevent",
				// UI wiring
				"button", "buttondisable", "buttonenable", "hide", "backlog", "automode",
				"rclick", "autosave", "alreadyread", "setonhidein", "setonhideout",
				"setonbacklogin", "setonbacklogout", "setonpush", "keyconfig",
				"writebacklog", "prohibit", "indent", "point", "scein", "sceout", "flip",
				// system bookkeeping
				"debug", "debugprint", "caption", "fontinit", "qsave", "qload", "menuload",
				"menusal", "saveinno", "dialog", "callnative", "exec", "pass", "reset",
				"wasmsync_list_bg", "wasmsync_list_cg", "wasmsync_list_bgm",
				"wasmsync_list_se", "alreadyread_mode", "autosave_off",
				// text helpers (inline markup is handled where the text is)
				"ruby", "ruby2", "rb", "rt", "/ruby", "/ruby2", "/rb", "/rt",
			};
			for(const char* entry : list)
				if(name == entry)
					return true;
			return false;
		}
	}

	Vm::Vm(const Config& config) : m_config(config)
	{
		m_vars["g.screen_width"] = m_config.stage != nullptr
			? std::to_string(m_config.stage->width())
			: "1024";
		m_vars["g.screen_height"] = m_config.stage != nullptr
			? std::to_string(m_config.stage->height())
			: "698";
	}

	// ---------------------------------------------------------------------------
	// scripts
	// ---------------------------------------------------------------------------
	const IetScript* Vm::Script(const std::string& file)
	{
		std::string error;
		LoadScript(file, error);
		std::map<std::string, IetScript>::const_iterator found = m_scripts.find(
			NormalizePath(file));
		return found == m_scripts.end() ? nullptr : &found->second;
	}

	bool Vm::LoadScript(const std::string& file, std::string& error)
	{
		const std::string key = NormalizePath(file);
		if(m_scripts.find(key) != m_scripts.end())
			return true;
		if(m_config.assets == nullptr)
		{
			error = "no asset source for " + file;
			return false;
		}
		std::vector<uint8_t> bytes;
		std::string resolved, readError;
		if(!m_config.assets->Read(file, bytes, resolved, readError))
		{
			error = "scenario not found: " + file;
			return false;
		}
		IetScript script;
		const std::string text = DecodeScenarioText(bytes.data(), bytes.size());
		if(!IetScript::Parse(text, script, error))
		{
			error = file + ": " + error;
			return false;
		}
		script.SetName(file);
		m_scripts[key] = std::move(script);
		m_scriptOrder.push_back(key);
		return true;
	}

	std::string Vm::CurrentFile() const
	{
		return m_frames.empty() ? std::string() : m_frames.back().file;
	}

	int Vm::CurrentLine() const
	{
		if(m_frames.empty())
			return 0;
		std::map<std::string, IetScript>::const_iterator found = m_scripts.find(
			m_frames.back().file);
		if(found == m_scripts.end() || m_frames.back().index == 0)
			return 0;
		const size_t at = m_frames.back().index - 1;
		if(at >= found->second.items().size())
			return 0;
		return found->second.items()[at].line;
	}

	const IetScript* Vm::script(const std::string& file) const
	{
		std::map<std::string, IetScript>::const_iterator found = m_scripts.find(
			NormalizePath(file));
		return found == m_scripts.end() ? nullptr : &found->second;
	}

	std::vector<std::string> Vm::loadedScripts() const { return m_scriptOrder; }

	std::vector<std::string> Vm::macroNames() const
	{
		std::vector<std::string> out;
		for(const std::pair<const std::string, Macro>& item : m_macros)
			out.push_back(item.first);
		std::sort(out.begin(), out.end());
		return out;
	}

	std::string Vm::assetFailuresText() const
	{
		std::string out;
		for(const std::string& failure : m_assetFailures)
		{
			if(!out.empty())
				out += " | ";
			out += failure;
		}
		return out;
	}

	// ---------------------------------------------------------------------------
	// start / boot
	// ---------------------------------------------------------------------------
	bool Vm::Start(const std::string& file, const std::string& label, std::string& error)
	{
		// Remembered so a save can name where the player is.
		m_currentFile = file;
		m_currentLabel = label;
		if(!LoadScript(file, error))
			return false;
		const std::string key = NormalizePath(file);
		const IetScript* script = &m_scripts[key];
		size_t index = 0;
		if(!label.empty())
		{
			const int at = script->LabelIndex(label);
			if(at < 0)
			{
				error = file + ": no label *" + label;
				return false;
			}
			index = (size_t)at + 1;
		}
		m_frames.clear();
		m_ifStack.clear();
		m_chgStack.clear();
		m_paramScopes.clear();
		Frame frame;
		frame.file = key;
		frame.index = index;
		m_frames.push_back(frame);
		return true;
	}

	bool Vm::Boot(std::string& error)
	{
		if(m_config.bootFile.empty())
		{
			m_bootSummary = "boot skipped";
			return true;
		}
		// the engine's global macro library: the game's own Lua has the same
		// macroadd commented out, and without it [adv] has no definition
		if(m_config.assets != nullptr && m_config.assets->Exists("macro.iet"))
		{
			Item item;
			item.kind = Item::Kind::Command;
			item.text = "macroadd";
			TagAttr attr;
			attr.name = "file";
			attr.value = "macro.iet";
			item.attrs.push_back(attr);
			DoMacroAdd(item);
		}
		if(!Start(m_config.bootFile, "", error))
			return false;
		m_booting = true;
		for(int steps = 0; steps < 200000; steps++)
		{
			const Event event = Next();
			if(event.kind == Event::Kind::Stop)
				break;
			if(event.kind == Event::Kind::Error)
			{
				m_booting = false;
				error = event.error;
				return false;
			}
		}
		m_booting = false;
		m_bootSummary = "boot " + m_config.bootFile + ": " + std::to_string(m_stats.bootCommands) +
			" commands, " + std::to_string(m_macros.size()) + " macros, " +
			std::to_string(m_config.stage != nullptr ? m_config.stage->styleNames().size() : 0) +
			" message styles";
		// boot leaves its own entry frame behind; the scene starts on a clean stack
		m_frames.clear();
		m_ifStack.clear();
		m_chgStack.clear();
		m_paramScopes.clear();
		return true;
	}

	// ---------------------------------------------------------------------------
	// the loop
	// ---------------------------------------------------------------------------
	Vm::Event Vm::Next()
	{
		Event event;
		for(long long guard = 0; guard < 1000000; guard++)
		{
			if(m_frames.empty())
			{
				event.kind = Event::Kind::Stop;
				return event;
			}
			const std::string file = m_frames.back().file;
			const IetScript* script = Script(file);
			if(script == nullptr)
			{
				event.kind = Event::Kind::Error;
				event.error = file + ": cannot load the script";
				return event;
			}
			if(m_frames.back().index >= script->items().size())
			{
				std::string returnError;
				Return(returnError);
				continue;
			}
			const size_t index = m_frames.back().index;
			const Item& item = script->items()[index];
			m_frames.back().index = index + 1;
			Event produced = Execute(item, index + 1);
			if(produced.kind != Event::Kind::None)
				return produced;
		}
		event.kind = Event::Kind::Error;
		event.error = "the script ran without reaching a page";
		return event;
	}

	void Vm::Return(std::string&)
	{
		if(m_frames.empty())
			return;
		const Frame frame = m_frames.back();
		m_frames.pop_back();
		while(m_paramScopes.size() > frame.paramDepth)
			m_paramScopes.pop_back();
		while(m_ifStack.size() > frame.ifDepth)
			m_ifStack.pop_back();
		while(m_chgStack.size() > frame.chgDepth)
			m_chgStack.pop_back();
	}

	bool Vm::PushMacro(const std::string& name, const Item& item, std::string& error)
	{
		std::map<std::string, Macro>::const_iterator found = m_macros.find(name);
		if(found == m_macros.end())
		{
			error = "macro not found: " + name;
			return false;
		}
		Frame frame;
		frame.file = found->second.file;
		frame.index = found->second.index + 1;
		frame.macro = name;
		frame.chgDepth = m_chgStack.size();
		frame.ifDepth = m_ifStack.size();
		frame.paramDepth = m_paramScopes.size();
		m_frames.push_back(frame);
		std::map<std::string, std::string> params;
		for(const TagAttr& attr : item.attrs)
			params[attr.name] = ExpandValue(attr.value, *this);
		m_paramScopes.push_back(params);
		m_stats.macroCalls++;
		m_macroUses[name]++;
		return true;
	}

	bool Vm::PushCall(const std::string& file, const std::string& label, const Item& item,
		std::string& error)
	{
		std::string targetFile = m_frames.empty() ? file : (file.empty() ? CurrentFile() : file);
		if(!LoadScript(targetFile, error))
			return false;
		const IetScript* script = Script(targetFile);
		if(script == nullptr)
		{
			error = "cannot load " + targetFile;
			return false;
		}
		size_t index = 0;
		if(!label.empty())
		{
			const int at = script->LabelIndex(label);
			if(at < 0)
			{
				error = targetFile + ": no label *" + label;
				return false;
			}
			index = (size_t)at + 1;
		}
		Frame frame;
		frame.file = NormalizePath(targetFile);
		frame.index = index;
		frame.macro = "call";
		frame.chgDepth = m_chgStack.size();
		frame.ifDepth = m_ifStack.size();
		frame.paramDepth = m_paramScopes.size();
		m_frames.push_back(frame);
		std::map<std::string, std::string> params;
		for(const TagAttr& attr : item.attrs)
			if(attr.name != "file" && attr.name != "label")
				params[attr.name] = ExpandValue(attr.value, *this);
		m_paramScopes.push_back(params);
		return true;
	}

	void Vm::Jump(const std::string& file, const std::string& label, std::string& error)
	{
		if(m_frames.empty())
		{
			error = "jump without a frame";
			return;
		}
		std::string targetFile = m_frames.back().file;
		if(!file.empty())
		{
			if(!LoadScript(file, error))
				return;
			targetFile = NormalizePath(file);
		}
		const IetScript* script = Script(targetFile);
		if(script == nullptr)
		{
			error = "cannot load " + targetFile;
			return;
		}
		size_t index = 0;
		if(!label.empty())
		{
			const int at = script->LabelIndex(label);
			if(at < 0)
			{
				error = targetFile + ": no label *" + label;
				return;
			}
			index = (size_t)at + 1;
		}
		m_frames.back().file = targetFile;
		m_frames.back().index = index;
	}

	// ---------------------------------------------------------------------------
	// conditions
	// ---------------------------------------------------------------------------
	bool Vm::Condition(const Item& item, bool& ok)
	{
		const TagAttr* attr = item.Attr("estimate");
		if(attr == nullptr)
		{
			// [if] without a condition never runs (a real file always has one)
			const TagAttr* plain = item.Attr("condition");
			if(plain == nullptr)
			{
				ok = true;
				return false;
			}
			attr = plain;
		}
		ok = true;
		m_stats.conditions++;
		bool good = true;
		const bool result = EvaluateCondition(attr->value, *this, good);
		if(!good)
		{
			m_stats.conditionErrors++;
			Warn(CurrentFile() + ": cannot evaluate \"" + attr->value + "\"");
		}
		return result;
	}

	size_t Vm::SkipToBranch(size_t start, bool& taken, std::string& error)
	{
		taken = false;
		if(m_frames.empty())
			return start;
		const IetScript* script = Script(m_frames.back().file);
		if(script == nullptr)
		{
			error = "cannot load " + m_frames.back().file;
			return start;
		}
		const std::vector<Item>& items = script->items();
		int depth = 0;
		for(size_t i = start; i < items.size(); i++)
		{
			const Item& item = items[i];
			if(item.kind != Item::Kind::Command)
				continue;
			if(item.text == "if")
			{
				depth++;
				continue;
			}
			if(item.text == "/if" || item.text == "endif")
			{
				if(depth == 0)
					return i + 1;
				depth--;
				continue;
			}
			if(depth != 0)
				continue;
			if(item.text == "elseif")
			{
				bool ok = true;
				if(Condition(item, ok))
				{
					m_ifStack.push_back(true);
					taken = true;
					return i + 1;
				}
				continue;
			}
			if(item.text == "else")
			{
				m_ifStack.push_back(true);
				taken = true;
				return i + 1;
			}
		}
		return items.size();
	}

	size_t Vm::SkipToEndIf(size_t start)
	{
		if(m_frames.empty())
			return start;
		const IetScript* script = Script(m_frames.back().file);
		if(script == nullptr)
			return start;
		const std::vector<Item>& items = script->items();
		int depth = 0;
		for(size_t i = start; i < items.size(); i++)
		{
			const Item& item = items[i];
			if(item.kind != Item::Kind::Command)
				continue;
			if(item.text == "if")
				depth++;
			else if(item.text == "/if" || item.text == "endif")
			{
				if(depth == 0)
					return i + 1;
				depth--;
			}
		}
		return items.size();
	}

	// ---------------------------------------------------------------------------
	// values
	// ---------------------------------------------------------------------------
	std::string Vm::Get(const std::string& name) const
	{
		for(size_t i = m_paramScopes.size(); i > 0; i--)
		{
			std::map<std::string, std::string>::const_iterator found =
				m_paramScopes[i - 1].find(name);
			if(found != m_paramScopes[i - 1].end())
				return found->second;
		}
		std::map<std::string, std::string>::const_iterator found = m_vars.find(name);
		return found == m_vars.end() ? std::string() : found->second;
	}

	std::string Vm::Value(const Item& item, const std::string& attr,
		const std::string& fallback) const
	{
		const TagAttr* found = item.Attr(attr);
		if(found == nullptr)
			return fallback;
		return ExpandValue(found->value, *this);
	}

	int Vm::IntValue(const Item& item, const std::string& attr, int fallback) const
	{
		const TagAttr* found = item.Attr(attr);
		if(found == nullptr)
			return fallback;
		const std::string value = ExpandValue(found->value, *this);
		if(value.empty())
			return fallback;
		return atoi(value.c_str());
	}

	std::string& Vm::TextBuffer()
	{
		return m_textTarget == "name" ? m_pageSpeaker : m_pageText;
	}

	// ---------------------------------------------------------------------------
	// text
	// ---------------------------------------------------------------------------
	void Vm::AppendText(const Item& item)
	{
		std::string& buffer = TextBuffer();
		const std::string file = CurrentFile();
		if(file == m_lastTextFile && m_lastTextLine > 0 && item.line > m_lastTextLine &&
			!buffer.empty() && buffer.back() != '\n')
			buffer += '\n';
		m_lastTextFile = file;
		m_lastTextLine = item.line;
		for(char c : item.text)
			buffer += (c == '@') ? '\n' : c;
	}

	Vm::Event Vm::FinishPage(const Item& item)
	{
		Event event;
		event.file = CurrentFile();
		event.line = item.line;
		if(m_textTarget == "name")
		{
			// a page break inside the name layer clears the name, it does not ask
			// the reader for anything; this is how [n] (no name box) works
			m_pageSpeaker.clear();
			return event;
		}
		const std::string text = Trim(m_pageText);
		if(text.empty())
			return event; // nothing on screen to wait for
		m_pageText.clear();
		m_lastTextLine = 0;
		m_stats.pages++;
		event.kind = Event::Kind::Page;
		event.text = text;
		event.speaker = m_pageSpeaker;
		return event;
	}

	// ---------------------------------------------------------------------------
	// diagnostics
	// ---------------------------------------------------------------------------
	void Vm::Note(const std::string& tag, const Item& item, bool ignored)
	{
		if(ignored)
		{
			m_stats.ignored++;
			m_ignoredTags[tag]++;
			if(m_ignoredLocations.find(tag) == m_ignoredLocations.end())
				m_ignoredLocations[tag] = CurrentFile() + ":" + std::to_string(item.line);
			return;
		}
		m_stats.unknown++;
		m_unknownTags[tag]++;
		if(m_unknownLocations.find(tag) == m_unknownLocations.end())
			m_unknownLocations[tag] = CurrentFile() + ":" + std::to_string(item.line) + " " +
				item.Describe();
	}

	void Vm::Warn(const std::string& message)
	{
		if(m_config.verbose)
		{
			fprintf(stderr, "[vm] %s\n", message.c_str());
			fflush(stderr);
		}
	}

	// ---------------------------------------------------------------------------
	// commands that change state
	// ---------------------------------------------------------------------------
	Vm::Event Vm::DoVar(const Item& item)
	{
		const std::string system = Value(item, "system");
		const std::string name = Value(item, "name");
		if(system.empty())
		{
			if(!name.empty())
				m_vars[name] = Value(item, "data");
			return Event();
		}
		if(system == "delete")
			m_vars.erase(name);
		else if(system == "os")
			m_vars[name] = "macos";
		else if(system == "screen_width")
			m_vars[name] = std::to_string(m_config.stage != nullptr ? m_config.stage->width() : 1024);
		else if(system == "screen_height")
			m_vars[name] = std::to_string(m_config.stage != nullptr ? m_config.stage->height() : 698);
		else if(system == "file_exists")
			m_vars[name] = (m_config.assets != nullptr && m_config.assets->Exists(Value(item, "file")))
				? "1"
				: "0";
		else if(system == "var_exist")
		{
			const std::string target = Value(item, "target");
			m_vars[name] = Get(target).empty() ? "0" : "1";
		}
		else if(system == "get_layer_info")
		{
			int width = 0, height = 0;
			const bool found = m_config.stage != nullptr &&
				m_config.stage->LayerSize(Value(item, "id"), width, height);
			m_vars[name + ".width"] = found ? std::to_string(width) : "0";
			m_vars[name + ".height"] = found ? std::to_string(height) : "0";
		}
		else if(system == "explode")
		{
			const std::string source = Value(item, "source");
			const std::string delimiter = Value(item, "delimiter", ",");
			std::vector<std::string> parts;
			size_t at = 0;
			for(;;)
			{
				const size_t found = source.find(delimiter, at);
				parts.push_back(source.substr(at, found == std::string::npos ? found : found - at));
				if(found == std::string::npos)
					break;
				at = found + (delimiter.empty() ? 1 : delimiter.size());
			}
			m_vars[name + ".size"] = std::to_string(parts.size());
			for(size_t i = 0; i < parts.size(); i++)
				m_vars[name + "." + std::to_string(i)] = parts[i];
		}
		else
			Note("var system=" + system, item, true);
		return Event();
	}

	Vm::Event Vm::DoChgMsg(const Item& item)
	{
		const std::string id = Value(item, "id");
		m_chgStack.push_back(m_textTarget);
		m_textTarget = id.empty() ? "adv" : id;
		if(m_config.stage != nullptr)
			m_config.stage->SetActiveStyle(m_textTarget);
		return Event();
	}

	Vm::Event Vm::DoFont(const Item& item)
	{
		if(m_config.stage == nullptr)
			return Event();
		TextStyle& style = m_config.stage->Style(m_textTarget);
		uint8_t rgba[4] = { 0, 0, 0, 255 };
		for(const TagAttr& attr : item.attrs)
		{
			const std::string value = ExpandValue(attr.value, *this);
			if(attr.name == "face")
				style.face = value;
			else if(attr.name == "size")
				style.size = atoi(value.c_str());
			else if(attr.name == "left")
				style.left = atoi(value.c_str());
			else if(attr.name == "top")
				style.top = atoi(value.c_str());
			else if(attr.name == "width")
				style.width = atoi(value.c_str());
			else if(attr.name == "height")
				style.height = atoi(value.c_str());
			else if(attr.name == "color" && ParseColor(value, rgba))
			{
				style.color[0] = rgba[0];
				style.color[1] = rgba[1];
				style.color[2] = rgba[2];
			}
			else if(attr.name == "shadowcolor" && ParseColor(value, rgba))
			{
				style.shadowColor[0] = rgba[0];
				style.shadowColor[1] = rgba[1];
				style.shadowColor[2] = rgba[2];
			}
			else if(attr.name == "shadow")
				style.shadow = atoi(value.c_str()) != 0;
			else if(attr.name == "align")
				style.align = value == "center" ? 1 : (value == "right" ? 2 : 0);
			else if(attr.name == "spacetop" || attr.name == "spacemiddle" ||
				attr.name == "spacebottom")
				style.lineSpacing = 8; // the real file's spacing, close enough for a still
		}
		return Event();
	}

	Vm::Event Vm::DoLyC(const Item& item)
	{
		if(m_config.stage == nullptr || m_config.assets == nullptr)
			return Event();
		const std::string id = Value(item, "id");
		if(id.empty())
			return Event();
		const std::string file = Value(item, "file");
		if(!file.empty())
		{
			std::string error;
			m_stats.assetLoads++;
			if(!m_config.stage->SetLayerImage(id, file, *m_config.assets, *m_config.cache, error))
			{
				m_stats.assetFailures++;
				m_assetFailures.push_back(error);
				Warn(error);
			}
			return Event();
		}
		const std::string color = Value(item, "color");
		uint8_t rgba[4] = { 0, 0, 0, 255 };
		if(color.empty() || !ParseColor(color, rgba))
			return Event();
		m_config.stage->SetLayerColor(id, rgba, IntValue(item, "width", m_config.stage->width()),
			IntValue(item, "height", m_config.stage->height()));
		return Event();
	}

	Vm::Event Vm::DoLyProp(const Item& item)
	{
		if(m_config.stage == nullptr)
			return Event();
		const std::string id = Value(item, "id");
		if(id.empty())
			return Event();
		for(const TagAttr& attr : item.attrs)
		{
			if(attr.name == "id")
				continue;
			const std::string value = ExpandValue(attr.value, *this);
			if(!m_config.stage->SetProperty(id, attr.name, value))
				Note("lyprop " + attr.name, item, true);
		}
		return Event();
	}

	Vm::Event Vm::DoLyDel(const Item& item)
	{
		if(m_config.stage == nullptr)
			return Event();
		std::string id = Value(item, "id");
		// the release overrides lydel in Lua to accept a comma list; do the same
		size_t at = 0;
		while(at <= id.size())
		{
			const size_t comma = id.find(',', at);
			const std::string part = id.substr(at, comma == std::string::npos ? comma : comma - at);
			// the group, not just the id: a title screen's buttons live on 60.1..60.7
			if(!Trim(part).empty())
				m_config.stage->DeleteLayerTree(Trim(part));
			if(comma == std::string::npos)
				break;
			at = comma + 1;
		}
		return Event();
	}

	Vm::Event Vm::DoLyColor(const Item& item)
	{
		if(m_config.stage == nullptr)
			return Event();
		const std::string id = Value(item, "id", "0");
		const std::string color = Value(item, "color", "000000");
		uint8_t rgba[4] = { 0, 0, 0, 255 };
		if(!ParseColor(color, rgba))
			return Event();
		m_config.stage->SetLayerColor(id, rgba, m_config.stage->width(), m_config.stage->height());
		return Event();
	}

	// `[button id="60.1" base="forsystem/title/button" text="forsystem/title/start"
	//          file="scenario/scene.iet" label="start" clickablethreshold="128"]`
	//
	// A button is two layers: the plate (`base`) on the id the script later moves
	// with [lyprop], and the label (`text`) on `<id>.1`, which inherits the plate's
	// position because the layer model nests by dotted id.  Both are the game's own
	// images - nothing here invents a button.
	Vm::Event Vm::DoButton(const Item& item)
	{
		ClickRegion region;
		region.id = Value(item, "id");
		region.file = Value(item, "file");
		region.label = Value(item, "label");
		region.call = IntValue(item, "call", 0) != 0;
		region.threshold = IntValue(item, "clickablethreshold", 0);
		if(region.id.empty())
		{
			Note("button without an id", item, false);
			return Event();
		}
		if(m_config.stage != nullptr && m_config.assets != nullptr)
		{
			const std::string base = Value(item, "base");
			const std::string text = Value(item, "text");
			std::string error;
			if(!base.empty())
			{
				m_stats.assetLoads++;
				if(m_config.stage->SetLayerImage(region.id, base, *m_config.assets,
					*m_config.cache, error))
					region.hasImage = true;
				else
				{
					m_stats.assetFailures++;
					m_assetFailures.push_back(error);
					Warn(error);
				}
			}
			if(!text.empty())
			{
				m_stats.assetLoads++;
				if(!m_config.stage->SetLayerImage(region.id + ".1", text, *m_config.assets,
					*m_config.cache, error))
				{
					m_stats.assetFailures++;
					m_assetFailures.push_back(error);
					Warn(error);
				}
			}
		}
		m_regions.push_back(region);
		return Event();
	}

	// `[point left="76" top="58" width="371" height="50" label="tanabata"]`
	//
	// The chapter screen's click rectangles: no image, just a rectangle and where it
	// goes.  Kept in the same list as the buttons so the player has one hit-test.
	Vm::Event Vm::DoPoint(const Item& item)
	{
		ClickRegion region;
		region.left = IntValue(item, "left", 0);
		region.top = IntValue(item, "top", 0);
		region.width = IntValue(item, "width", 0);
		region.height = IntValue(item, "height", 0);
		region.label = Value(item, "label");
		region.file = Value(item, "file");
		region.id = item.Attr("id") != nullptr ? Value(item, "id") : std::string();
		region.hasImage = false;
		if(region.width <= 0 || region.height <= 0)
		{
			Note("point without a size", item, false);
			return Event();
		}
		m_regions.push_back(region);
		return Event();
	}

	namespace
	{
		// The words a choice shows for one label: the first text run after it, cut at
		// the first line break.  A script that puts no text there gets its label name
		// shown instead - the real title builds this string in Lua, and pretending
		// otherwise would be inventing dialogue.
		std::string FirstLineUnder(const IetScript& script, int labelIndex)
		{
			std::string out;
			for(size_t i = (size_t)labelIndex + 1; i < script.items().size(); i++)
			{
				const Item& item = script.items()[i];
				if(item.kind == Item::Kind::Label)
					break;
				if(item.kind != Item::Kind::Text)
					continue;
				out = StripInlineMarkup(item.text);
				for(char& c : out)
					if(c == '@')
						c = '\n';
				const size_t newline = out.find('\n');
				if(newline != std::string::npos)
					out = out.substr(0, newline);
				while(!out.empty() && (out.back() == ' ' || out.back() == '\t'))
					out.pop_back();
				if(!out.empty())
					return out;
			}
			return out;
		}
	}

	// `[select file="d02_ik01_1" pre="route_c_01" cond="g.selecteachroute"]`
	//
	// The options are the labels of the named script that start with `pre`.  This is
	// what the .ast corpus' select command says, read literally; the engine's own
	// choice UI is Lua (`calllua function=select_*` in system_script.asb), which is
	// reported as not modelled rather than guessed at.
	Vm::Event Vm::DoSelect(const Item& item)
	{
		Event event;
		const std::string file = Value(item, "file");
		const std::string prefix = Value(item, "pre");
		const std::string target = file.empty() ? NormalizePath(CurrentFile())
			: NormalizePath(file);
		std::string error;
		if(!LoadScript(target, error))
		{
			event.kind = Event::Kind::Error;
			event.error = error;
			return event;
		}
		const IetScript* script = Script(target);
		if(script == nullptr)
		{
			event.kind = Event::Kind::Error;
			event.error = "select: cannot read " + target;
			return event;
		}
		for(const std::string& label : script->labels())
		{
			if(!prefix.empty() && label.compare(0, prefix.size(), prefix) != 0)
				continue;
			Choice choice;
			choice.label = label;
			choice.text = FirstLineUnder(*script, script->LabelIndex(label));
			if(choice.text.empty())
				choice.text = label;
			choice.line = item.line;
			event.choices.push_back(choice);
		}
		if(event.choices.empty())
		{
			event.kind = Event::Kind::Error;
			event.error = "select: " + target + " has no label starting with \"" + prefix +
				"\"";
			return event;
		}
		event.kind = Event::Kind::Select;
		event.choiceFile = target;
		m_lastChoices = event.choices;
		m_lastChoiceFile = target;
		m_stats.conditions++; // a choice is a branch the run took
		return event;
	}

	bool Vm::ResolveChoice(int index, std::string& error)
	{
		error.clear();
		if(index < 0 || index >= (int)m_lastChoices.size())
		{
			error = "no option " + std::to_string(index);
			return false;
		}
		Jump(m_lastChoiceFile, m_lastChoices[index].label, error);
		return error.empty();
	}

	bool Vm::RegionBox(int index, int& left, int& top, int& width, int& height) const
	{
		if(index < 0 || index >= (int)m_regions.size())
			return false;
		const ClickRegion& region = m_regions[index];
		if(region.hasImage)
		{
			if(m_config.stage == nullptr)
				return false;
			// the rect comes from the stage: [lyprop] moved the plate after the button
			// was declared, so the tag's own numbers would be stale
			return m_config.stage->LayerBox(region.id, left, top, width, height);
		}
		left = region.left;
		top = region.top;
		width = region.width;
		height = region.height;
		return width > 0 && height > 0;
	}

	bool Vm::RegionHit(int index, int x, int y) const
	{
		if(index < 0 || index >= (int)m_regions.size())
			return false;
		int left = 0, top = 0, width = 0, height = 0;
		if(!RegionBox(index, left, top, width, height))
			return false;
		if(x < left || y < top || x >= left + width || y >= top + height)
			return false;
		const ClickRegion& region = m_regions[index];
		if(region.hasImage && region.threshold > 0 && m_config.stage != nullptr)
		{
			int alpha = 0;
			// the plate's transparent corners are not part of the button
			if(m_config.stage->LayerAlphaAt(region.id, x, y, alpha) &&
				alpha < region.threshold)
				return false;
		}
		return true;
	}

	Vm::Event Vm::DoTrans(const Item& item)
	{
		if(m_config.stage != nullptr)
			m_config.stage->NoteTransition(Value(item, "type", "1"), IntValue(item, "time", 0));
		return Event();
	}

	Vm::Event Vm::DoMacroAdd(const Item& item)
	{
		std::string error;
		const std::string file = Value(item, "file");
		if(!LoadScript(file, error))
		{
			Warn(error);
			m_stats.assetFailures++;
			m_assetFailures.push_back(error);
			return Event();
		}
		const IetScript* script = Script(file);
		if(script == nullptr)
			return Event();
		for(size_t i = 0; i < script->items().size(); i++)
		{
			const Item& entry = script->items()[i];
			if(entry.kind != Item::Kind::Label)
				continue;
			Macro macro;
			macro.file = NormalizePath(file);
			macro.index = i;
			macro.line = entry.line;
			m_macros[entry.text] = macro;
		}
		m_stats.macroAdds++;
		return Event();
	}

	Vm::Event Vm::DoLua(const Item& item)
	{
		const std::string function = Value(item, "function");
		std::string error;
		if(!RunLuaFunction(function, error))
			Note("calllua:" + (function.empty() ? std::string("?") : function), item, true);
		return Event();
	}

	// ---------------------------------------------------------------------------
	// the game's Lua, as a stub that keeps its tag calls
	// ---------------------------------------------------------------------------
	bool Vm::ExtractLuaTags(const std::string& body, std::vector<Item>& tags) const
	{
		size_t at = 0;
		while(at < body.size())
		{
			const size_t call = body.find("e:tag", at);
			if(call == std::string::npos)
				break;
			const size_t open = body.find('{', call);
			if(open == std::string::npos)
				break;
			const size_t close = MatchBrace(body, open);
			if(close == std::string::npos)
				return false;
			const std::string inside = body.substr(open + 1, close - open - 1);
			at = close + 1;

			// split the arguments at top-level commas
			std::vector<std::string> parts;
			std::string current;
			int depth = 0;
			for(size_t i = 0; i < inside.size(); i++)
			{
				const char c = inside[i];
				if(c == '"' || c == '\'')
				{
					const char quote = c;
					current += c;
					i++;
					while(i < inside.size() && inside[i] != quote)
					{
						if(inside[i] == '\\' && i + 1 < inside.size())
							current += inside[i++];
						current += inside[i++];
					}
					if(i < inside.size())
						current += inside[i];
					continue;
				}
				if(c == '(' || c == '{' || c == '[')
					depth++;
				else if(c == ')' || c == '}' || c == ']')
					depth--;
				if(c == ',' && depth == 0)
				{
					parts.push_back(current);
					current.clear();
					continue;
				}
				current += c;
			}
			if(!current.empty())
				parts.push_back(current);
			if(parts.empty())
				continue;
			Item tag;
			tag.kind = Item::Kind::Command;
			std::string name = Trim(parts[0]);
			if(name.size() >= 2 && (name.front() == '"' || name.front() == '\''))
				name = name.substr(1, name.size() - 2);
			tag.text = name;
			for(size_t i = 1; i < parts.size(); i++)
			{
				const std::string part = Trim(parts[i]);
				const size_t equals = part.find('=');
				if(equals == std::string::npos)
					continue;
				TagAttr attr;
				attr.name = Trim(part.substr(0, equals));
				std::string value = Trim(part.substr(equals + 1));
				if(value.size() >= 2 && (value.front() == '"' || value.front() == '\''))
					value = value.substr(1, value.size() - 2);
				else if(value.find("e:var(") != std::string::npos)
				{
					const size_t quote = value.find_first_of("\"'", value.find("e:var("));
					if(quote != std::string::npos)
					{
						const size_t end = value.find(value[quote], quote + 1);
						if(end != std::string::npos)
						{
							const std::string variable = value.substr(quote + 1, end - quote - 1);
							value = Get(variable);
						}
					}
				}
				else if(value.find("math.") != std::string::npos ||
					!value.empty() && !std::isdigit((unsigned char)value[0]))
					continue; // an expression we do not model: leave the attribute out
				attr.value = value;
				tag.attrs.push_back(attr);
			}
			tags.push_back(tag);
		}
		return true;
	}

	bool Vm::RunLuaFunction(const std::string& function, std::string& error)
	{
		if(function.empty())
		{
			error = "calllua without a function";
			return false;
		}
		std::string source;
		for(const std::string& key : m_scriptOrder)
		{
			source += m_scripts[key].lua();
			source += "\n";
		}
		// drop Lua comments and keep string literals intact: the game has whole
		// macroadds commented out, and running those would invent macros
		std::string clean;
		clean.reserve(source.size());
		for(size_t i = 0; i < source.size();)
		{
			if(source.compare(i, 4, "--[[") == 0)
			{
				const size_t end = source.find("]]", i + 4);
				i = end == std::string::npos ? source.size() : end + 2;
				continue;
			}
			if(source.compare(i, 2, "--") == 0)
			{
				while(i < source.size() && source[i] != '\n')
					i++;
				continue;
			}
			if(source[i] == '"' || source[i] == '\'')
			{
				const char quote = source[i];
				clean += source[i++];
				while(i < source.size() && source[i] != quote)
				{
					if(source[i] == '\\' && i + 1 < source.size())
					{
						clean += source[i++];
						clean += source[i++];
						continue;
					}
					clean += source[i++];
				}
				if(i < source.size())
					clean += source[i++];
				continue;
			}
			clean += source[i++];
		}
		const std::string needle = "function " + function;
		const size_t at = clean.find(needle);
		if(at == std::string::npos)
		{
			error = "lua function not in the loaded scripts: " + function;
			return false;
		}
		const size_t signatureEnd = clean.find(')', at);
		if(signatureEnd == std::string::npos)
		{
			error = "lua function has no parameter list: " + function;
			return false;
		}
		const size_t end = LuaBlockEnd(clean, signatureEnd);
		if(end == std::string::npos)
		{
			error = "unbalanced lua body: " + function;
			return false;
		}
		std::vector<Item> tags;
		if(!ExtractLuaTags(clean.substr(signatureEnd, end - signatureEnd), tags))
		{
			error = "cannot read the tag calls of " + function;
			return false;
		}
		for(const Item& tag : tags)
		{
			const Event produced = Execute(tag, 0);
			if(produced.kind == Event::Kind::Stop ||
				produced.kind == Event::Kind::Error)
				break;
		}
		m_stats.luaTags += (long long)tags.size();
		return true;
	}

	// ---------------------------------------------------------------------------
	// dispatch
	// ---------------------------------------------------------------------------
	Vm::Event Vm::Execute(const Item& item, size_t nextIndex)
	{
		Event event;
		if(item.kind == Item::Kind::Label)
			return event; // falling into a label is not an action
		if(item.kind == Item::Kind::Text)
		{
			AppendText(item);
			return event;
		}
		m_stats.commands++;
		if(m_booting)
			m_stats.bootCommands++;
		const std::string& name = item.text;
		if(name.empty())
			return event;

		const bool inlineTag = name == "ruby" || name == "ruby2" || name == "rb" || name == "rt" ||
			name == "/ruby" || name == "/ruby2" || name == "/rb" || name == "/rt";
		if(inlineTag)
		{
			m_stats.inlineTags++;
			return event;
		}

		// --- pages -----------------------------------------------------------
		if(name == "rp")
			return FinishPage(item);
		if(name == "print")
		{
			TextBuffer() += Value(item, "data");
			return event;
		}
		if(name == "wait")
		{
			m_stats.waits++;
			m_stats.waitMs += IntValue(item, "time", 0);
			return event;
		}

		// --- control flow ----------------------------------------------------
		if(name == "if")
		{
			bool ok = true;
			if(Condition(item, ok))
			{
				m_ifStack.push_back(true);
				return event;
			}
			bool taken = false;
			std::string error;
			const size_t target = SkipToBranch(nextIndex, taken, error);
			if(!m_frames.empty())
				m_frames.back().index = target;
			(void)taken;
			return event;
		}
		if(name == "elseif" || name == "else")
		{
			if(!m_ifStack.empty() && m_ifStack.back())
			{
				// this chain already ran a branch: skip the rest of it
				const size_t target = SkipToEndIf(nextIndex);
				m_ifStack.pop_back();
				if(!m_frames.empty())
					m_frames.back().index = target;
				return event;
			}
			if(name == "else")
				m_ifStack.push_back(true);
			return event;
		}
		if(name == "/if" || name == "endif")
		{
			if(!m_ifStack.empty())
				m_ifStack.pop_back();
			return event;
		}
		if(name == "jump")
		{
			if(m_booting && m_frames.size() == 1)
			{
				event.kind = Event::Kind::Stop;
				return event;
			}
			std::string error;
			Jump(Value(item, "file"), Value(item, "label"), error);
			if(!error.empty())
			{
				event.kind = Event::Kind::Error;
				event.error = error;
			}
			return event;
		}
		if(name == "call")
		{
			std::string error;
			if(!PushCall(Value(item, "file"), Value(item, "label"), item, error))
			{
				event.kind = Event::Kind::Error;
				event.error = error;
			}
			return event;
		}
		if(name == "return")
		{
			std::string error;
			Return(error);
			return event;
		}
		if(name == "stop" || name == "exit" || name == "reset")
		{
			event.kind = Event::Kind::Stop;
			return event;
		}
		if(name == "calllua")
			return DoLua(item);

		// --- configuration and layers ---------------------------------------
		if(name == "var")
			return DoVar(item);
		if(name == "chgmsg")
			return DoChgMsg(item);
		if(name == "/chgmsg")
		{
			if(!m_chgStack.empty())
			{
				m_textTarget = m_chgStack.back();
				m_chgStack.pop_back();
			}
			if(m_config.stage != nullptr)
				m_config.stage->SetActiveStyle(m_textTarget);
			return event;
		}
		if(name == "font")
			return DoFont(item);
		if(name == "macroadd")
			return DoMacroAdd(item);
		if(name == "lyc")
			return DoLyC(item);
		if(name == "lyprop")
			return DoLyProp(item);
		if(name == "lydel")
			return DoLyDel(item);
		if(name == "lycolor")
			return DoLyColor(item);
		if(name == "trans")
			return DoTrans(item);
		if(name == "button")
			return DoButton(item);
		if(name == "point")
			return DoPoint(item);
		if(name == "select")
			return DoSelect(item);
		if(name == "flip")
			return event; // the double buffer is not modelled: the canvas is the frame

		// --- macros ----------------------------------------------------------
		if(m_macros.find(name) != m_macros.end())
		{
			std::string error;
			if(!PushMacro(name, item, error))
			{
				event.kind = Event::Kind::Error;
				event.error = error;
			}
			return event;
		}

		// --- handed to the front end -----------------------------------------
		if(m_sink != nullptr && IsFrontEndTag(name))
		{
			TagEvent tag;
			tag.name = name;
			tag.file = CurrentFile();
			tag.line = item.line;
			for(const TagAttr& attr : item.attrs)
				tag.attributes[attr.name] = ExpandValue(attr.value, *this);
			m_frontEndTags[name]++;
			m_sink->OnTag(tag);
			return event;
		}

		// --- known but not acted on -----------------------------------------
		if(IsKnownIgnored(name))
		{
			Note(name, item, true);
			return event;
		}
		Note(name, item, false);
		return event;
	}
}
