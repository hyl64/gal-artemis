// astscene.cpp - walking a shipped .ast and turning it into a picture

#include "astscene.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <set>
#include <sys/stat.h>

namespace artemis
{
	namespace
	{
		std::string IntText(long long value)
		{
			char buffer[32];
			snprintf(buffer, sizeof buffer, "%lld", value);
			return buffer;
		}

		// a stable short fingerprint of a string, so a report can say "the same
		// line" or "a different line" without carrying the line around
		std::string Fingerprint(const std::string& text)
		{
			uint32_t hash = 2166136261u;
			for(unsigned char c : text)
			{
				hash ^= c;
				hash *= 16777619u;
			}
			char buffer[16];
			snprintf(buffer, sizeof buffer, "%08x", hash);
			return buffer;
		}

		// commands are spelled with a leading `{` and no quotes in the source, and
		// the attribute order is the file's; the report keeps both so a reader can
		// find the line again
		std::string DescribeCommand(const AstCommand& command, size_t maxValue, bool showText)
		{
			std::string out = "{\"" + command.name + "\"";
			for(const AstAttribute& attribute : command.attributes)
			{
				out += ", " + attribute.name + "=";
				if(attribute.quoted)
				{
					std::string value = attribute.value;
					bool ascii = true;
					for(unsigned char c : value)
						if(c >= 0x80)
							ascii = false;
					if(!ascii && !showText)
					{
						// a scene title or a name table: the shape of the command is
						// wanted, the words are not.  ASCII values - file names, paths,
						// easings - are the ones that matter for reading the command.
						value = "<utf8 " + std::to_string(value.size()) + "B " +
							Fingerprint(value) + ">";
					}
					else if(value.size() > maxValue)
					{
						// cut on a code point boundary, never inside one
						size_t cut = maxValue;
						while(cut > 0 && (value[cut] & 0xc0) == 0x80)
							cut--;
						value = value.substr(0, cut) + "...";
					}
					out += "\"" + value + "\"";
				}
				else
					out += attribute.value;
			}
			out += "}";
			return out;
		}


		int CountUtf8Chars(const std::string& text)
		{
			int count = 0;
			for(unsigned char c : text)
				if((c & 0xc0) != 0x80)
					count++;
			return count;
		}

		std::string Attr(const std::vector<AstAttribute>& attributes, const char* name,
			const std::string& fallback = std::string())
		{
			for(const AstAttribute& attribute : attributes)
				if(attribute.name == name)
					return attribute.value;
			return fallback;
		}

		int IntAttr(const std::vector<AstAttribute>& attributes, const char* name,
			int fallback)
		{
			for(const AstAttribute& attribute : attributes)
				if(attribute.name == name)
					return atoi(attribute.value.c_str());
			return fallback;
		}

		bool HasAttr(const std::vector<AstAttribute>& attributes, const char* name)
		{
			for(const AstAttribute& attribute : attributes)
				if(attribute.name == name)
					return true;
			return false;
		}

		std::string BlockName(const AstBlock& block, int index)
		{
			if(!block.name.empty())
				return block.name;
			return "#" + std::to_string(index);
		}
	}

	// ---------------------------------------------------------------------------
	// asset naming
	// ---------------------------------------------------------------------------
	//
	// The scripts name assets the way the engine's little path language spells
	// them: `path=":bg/" file="pink"` is `image/bg/pink`.  Only the four roots the
	// corpus actually uses need a translation; an unknown `:x/` simply loses its
	// colon, which is right for `:bgimage/` and wrong for nothing in the corpus.
	std::string ResolveAstAsset(const std::string& path, const std::string& file)
	{
		if(file.empty())
			return std::string();
		if(path.empty())
			return file;
		std::string root = path;
		if(!root.empty() && root[0] == ':')
			root.erase(root.begin());
		for(char& c : root)
			if(c == '\\')
				c = '/';
		while(!root.empty() && root.front() == '/')
			root.erase(root.begin());
		while(!root.empty() && root.back() == '/')
			root.pop_back();
		// the roots a real title uses, measured over 59 scripts: :bg/ :cg/ :ev/ :fg/
		// :sys/ :thumb/ and the per-character :fg/<ch>/<size>/ form
		static const struct { const char* from; const char* to; } map[] = {
			{ "bg", "image/bg" },
			{ "cg", "image/cg" },
			{ "ev", "image/ev" },
			{ "evmask", "image/evmask" },
			{ "fg", "image/fg" },
			{ "thumb", "image/thumb" },
			{ "sys", "system/image" },
			{ "se", "sound/se" },
			{ "vo", "sound/vo" },
			{ "bgv", "sound/bgv" },
			{ "bgm", "sound/bgm" },
		};
		for(const auto& entry : map)
		{
			const std::string from = entry.from;
			if(root == from || root.compare(0, from.size() + 1, from + "/") == 0)
			{
				const std::string tail = root.size() > from.size() ? root.substr(from.size()) : "";
				return std::string(entry.to) + tail + "/" + file;
			}
		}
		return root + "/" + file;
	}

	bool AstImageAllowed(const std::string& logical)
	{
		std::string name = logical;
		for(char& c : name)
			if(c == '\\')
				c = '/';
		while(!name.empty() && name.front() == '/')
			name.erase(name.begin());
		// the adult corpus' event, CG and character art: never read, by policy
		static const char* denied[] = { "image/ev/", "image/cg/", "image/fg/", "image/evmask/" };
		for(const char* prefix : denied)
		{
			const std::string p = prefix;
			if(name.compare(0, p.size(), p) == 0)
				return false;
		}
		return true;
	}

	// ---------------------------------------------------------------------------
	// the chain
	// ---------------------------------------------------------------------------
	AstChainInfo WalkAstChain(const LuaAst& ast)
	{
		AstChainInfo info;
		info.chained = !ast.arrayed;
		const int count = (int)ast.blocks.size();
		if(count == 0)
		{
			info.error = "the file has no blocks";
			return info;
		}

		if(!info.chained)
		{
			// the arrayed layout has no links at all: the file order is the order
			for(int i = 0; i < count; i++)
				info.order.push_back(i);
			info.head = BlockName(ast.blocks[0], 0);
			info.heads.push_back(info.head);
			info.ok = true;
			return info;
		}

		std::map<std::string, int> byName;
		for(int i = 0; i < count; i++)
		{
			const AstBlock& block = ast.blocks[i];
			if(block.name.empty())
			{
				info.unnamed++;
				continue;
			}
			if(byName.find(block.name) != byName.end())
			{
				// a duplicate name makes every link to it ambiguous: say so
				if(info.error.empty())
					info.error = "block name " + block.name + " is defined twice";
			}
			byName[block.name] = i;
		}

		std::set<std::string> targets;
		for(const AstBlock& block : ast.blocks)
			if(!block.next.empty())
			{
				info.withNext++;
				targets.insert(block.next);
			}

		// a chain head is a block nothing links to.  The first one in file order is
		// where a run starts; the rest are branch targets, and they are reported.
		for(int i = 0; i < count; i++)
			if(!ast.blocks[i].name.empty() && targets.find(ast.blocks[i].name) == targets.end())
				info.heads.push_back(ast.blocks[i].name);
		if(info.heads.empty())
		{
			info.error = "every block is a linknext target: the file has no chain head";
			return info;
		}
		info.head = info.heads.front();

		std::set<int> seen;
		std::string current = info.head;
		while(!current.empty())
		{
			std::map<std::string, int>::const_iterator found = byName.find(current);
			if(found == byName.end())
			{
				info.broken++;
				if(info.error.empty())
					info.error = "linknext points at " + current + ", which the file does not define";
				break;
			}
			const int index = found->second;
			if(seen.find(index) != seen.end())
			{
				info.repeated++;
				if(info.error.empty())
					info.error = "the linknext chain returns to " + current;
				break;
			}
			seen.insert(index);
			info.order.push_back(index);
			current = ast.blocks[index].next;
		}

		for(int i = 0; i < count; i++)
		{
			if(seen.find(i) != seen.end())
				continue;
			if(ast.blocks[i].name.empty())
				continue; // already counted as unnamed
			info.unreached.push_back(ast.blocks[i].name);
		}

		info.ok = info.error.empty() && info.repeated == 0 && info.broken == 0 &&
			info.unreached.empty();
		if(!info.ok && info.error.empty())
			info.error = IntText((long long)info.unreached.size()) +
				" block(s) are not on the chain from " + info.head;
		return info;
	}

	// ---------------------------------------------------------------------------
	// --ast-info
	// ---------------------------------------------------------------------------
	std::string FormatAstInfo(const LuaAst& ast, const AstChainInfo& chain,
		const AstInfoOptions& options)
	{
		std::string out;
		std::map<std::string, long long> commands;
		std::set<std::string> voiceFiles;
		long long lineCount = 0;
		long long voiceCount = 0;
		for(const AstBlock& block : ast.blocks)
		{
			for(const AstCommand& command : block.commands)
				commands[command.name]++;
			lineCount += (long long)block.lines.size();
			voiceCount += (long long)block.voices.size();
			for(const std::string& voice : block.voices)
				voiceFiles.insert(voice);
		}
		long long commandCount = 0;
		for(const std::pair<const std::string, long long>& item : commands)
			commandCount += item.second;

		out += "layout=" + std::string(chain.chained ? "chained" : "arrayed") + "\n";
		out += "astver=" + (ast.version.empty() ? "unknown" : ast.version) + "\n";
		out += "blocks=" + std::to_string(ast.blocks.size()) + "\n";
		out += "commands=" + IntText(commandCount) + "\n";
		out += "lines=" + IntText(lineCount) + "\n";
		out += "voices=" + IntText(voiceCount) + "\n";
		out += "voice_files=" + std::to_string(voiceFiles.size()) + "\n";
		out += "distinct_commands=" + std::to_string(commands.size()) + "\n";
		for(const std::pair<const std::string, long long>& item : commands)
			out += "command." + item.first + "=" + IntText(item.second) + "\n";

		out += "chain_blocks_with_linknext=" + std::to_string(chain.withNext) + "\n";
		out += "chain_head=" + (chain.head.empty() ? "none" : chain.head) + "\n";
		out += "chain_heads=" + std::to_string(chain.heads.size()) + "\n";
		out += "chain_walked=" + std::to_string(chain.order.size()) + "\n";
		out += "chain_repeated=" + std::to_string(chain.repeated) + "\n";
		out += "chain_broken=" + std::to_string(chain.broken) + "\n";
		out += "chain_unreached=" + std::to_string(chain.unreached.size()) + "\n";
		out += "chain_ok=" + std::to_string(chain.ok ? 1 : 0) + "\n";
		if(!chain.error.empty())
			out += "chain_error=" + chain.error + "\n";

		// the first block of the chain: its first commands verbatim (they name
		// assets and numbers, not dialogue) and the first lines *masked* - how many
		// characters and a fingerprint, which proves the words were parsed without
		// copying them into a log
		if(!chain.order.empty())
		{
			const AstBlock& first = ast.blocks[chain.order.front()];
			out += "first_block=" + BlockName(first, chain.order.front()) + "\n";
			out += "first_commands=" + std::to_string(first.commands.size()) + "\n";
			out += "first_lines=" + std::to_string(first.lines.size()) + "\n";
			for(size_t i = 0; i < first.commands.size() && (int)i < options.preview; i++)
				out += "preview.command." + std::to_string(i) + "=" +
					DescribeCommand(first.commands[i], 24, options.showText) + "\n";
			for(size_t i = 0; i < first.lines.size() && (int)i < options.preview; i++)
			{
				const AstLine& line = first.lines[i];
				out += "preview.line." + std::to_string(i) + "=";
				out += "speaker=" + (line.speaker.empty() ? "-" : "\"" + line.speaker + "\"");
				out += " chars=" + std::to_string(CountUtf8Chars(line.text));
				out += " hash=" + Fingerprint(line.text);
				if(options.showText)
					out += " text=\"" + line.text + "\"";
				out += "\n";
			}
		}
		if(!ast.notes.empty())
			for(size_t i = 0; i < ast.notes.size(); i++)
				out += "note." + std::to_string(i) + "=" + ast.notes[i] + "\n";
		out += "ok=1\n";
		return out;
	}

	void AddToCorpus(AstCorpusStats& stats, const LuaAst& ast, const AstChainInfo& chain)
	{
		stats.files++;
		stats.blocks += (long long)ast.blocks.size();
		if(chain.chained)
			stats.chained++;
		else
			stats.arrayed++;
		if(!chain.ok)
			stats.chainDefects++;
		for(const AstBlock& block : ast.blocks)
		{
			stats.commands += (long long)block.commands.size();
			stats.lines += (long long)block.lines.size();
			stats.voices += (long long)block.voices.size();
			for(const AstCommand& command : block.commands)
				stats.commandNames[command.name]++;
		}
	}

	std::string FormatAstCorpus(const AstCorpusStats& stats)
	{
		std::string out;
		out += "corpus.files=" + std::to_string(stats.files) + "\n";
		out += "corpus.failed=" + std::to_string(stats.failed) + "\n";
		out += "corpus.chained=" + IntText(stats.chained) + "\n";
		out += "corpus.arrayed=" + IntText(stats.arrayed) + "\n";
		out += "corpus.blocks=" + IntText(stats.blocks) + "\n";
		out += "corpus.commands=" + IntText(stats.commands) + "\n";
		out += "corpus.lines=" + IntText(stats.lines) + "\n";
		out += "corpus.voices=" + IntText(stats.voices) + "\n";
		out += "corpus.chain_defects=" + IntText(stats.chainDefects) + "\n";
		for(const std::pair<const std::string, long long>& item : stats.commandNames)
			out += "corpus.command." + item.first + "=" + IntText(item.second) + "\n";
		for(size_t i = 0; i < stats.failures.size(); i++)
			out += "corpus.failure." + std::to_string(i) + "=" + stats.failures[i] + "\n";
		return out;
	}

	// ---------------------------------------------------------------------------
	// events
	// ---------------------------------------------------------------------------
	std::string SceneEvent::Str(const std::string& key, const std::string& fallback) const
	{
		std::map<std::string, std::string>::const_iterator found = attrs.find(key);
		return found == attrs.end() ? fallback : found->second;
	}

	int SceneEvent::Int(const std::string& key, int fallback) const
	{
		std::map<std::string, std::string>::const_iterator found = attrs.find(key);
		return found == attrs.end() || found->second.empty() ? fallback
			: atoi(found->second.c_str());
	}

	bool SceneEvent::Has(const std::string& key) const
	{
		return attrs.find(key) != attrs.end();
	}

	// ---------------------------------------------------------------------------
	// the run
	// ---------------------------------------------------------------------------
	AstRun::AstRun(const LuaAst& ast, const Config& config)
		: m_ast(ast), m_config(config), m_chain(WalkAstChain(ast))
	{
		if(!m_chain.ok && m_config.stopAtBrokenChain)
			m_error = m_chain.error;
		if(!m_config.startBlock.empty())
		{
			// a chapter jump inside the same file: the cursor moves, the chain and
			// its checks stay what they are
			bool found = false;
			for(size_t i = 0; i < m_chain.order.size(); i++)
			{
				const AstBlock& block = m_ast.blocks[m_chain.order[i]];
				if(block.name == m_config.startBlock)
				{
					m_cursor = i;
					found = true;
					break;
				}
			}
			if(!found && m_error.empty())
				m_error = "the chain has no block named " + m_config.startBlock;
		}
	}

	bool AstRun::AtEnd() const
	{
		return m_cursor >= m_chain.order.size();
	}

	std::vector<SceneEvent> AstRun::TakeEvents()
	{
		std::vector<SceneEvent> out;
		out.swap(m_events);
		return out;
	}

	std::string AstRun::currentBlockName() const
	{
		if(m_cursor == 0 || m_cursor > m_chain.order.size())
			return std::string();
		const int index = m_chain.order[m_cursor - 1];
		return BlockName(m_ast.blocks[index], index);
	}

	bool AstRun::ApplyBackground(const AstCommand& command, std::string& error)
	{
		error.clear();
		const std::string logical = ResolveAstAsset(Attr(command.attributes, "path"),
			Attr(command.attributes, "file"));
		if(logical.empty())
		{
			error = "bg with no file";
			return false;
		}
		if(!AstImageAllowed(logical))
		{
			m_stats.policySkipped++;
			m_stats.policy["bg"]++;
			return false;
		}
		if(m_config.stage == nullptr || m_config.assets == nullptr || m_config.cache == nullptr)
			return false;
		const std::string id = Attr(command.attributes, "id", "0");
		if(!m_config.stage->SetLayerImage(id, logical, *m_config.assets, *m_config.cache, error))
		{
			m_stats.assetFailures++;
			m_stats.assetFailuresText.push_back(logical + ": " + error);
			return false;
		}
		if(HasAttr(command.attributes, "x"))
			m_config.stage->SetProperty(id, "left", Attr(command.attributes, "x", "0"));
		if(HasAttr(command.attributes, "y"))
			m_config.stage->SetProperty(id, "top", Attr(command.attributes, "y", "0"));
		if(HasAttr(command.attributes, "alpha"))
			m_config.stage->SetProperty(id, "alpha", Attr(command.attributes, "alpha", "255"));
		m_config.stage->NoteTransition("bg", IntAttr(command.attributes, "time", 0));
		m_stats.stageOps++;
		return true;
	}

	// What a command does, in one place.  The rule this table follows: a command
	// is either applied to the stage, queued as an event for the front end, booked
	// as script state we can follow, refused by the image policy, or counted as
	// unwired - and every one of those ends up in the report with its position.
	void AstRun::ApplyCommand(const AstCommand& command, const AstBlock& block, int blockIndex,
		int index)
	{
		const std::string& name = command.name;
		SceneEvent event;
		event.command = name;
		event.block = blockIndex;
		event.index = index;
		event.line = block.line;
		event.order = m_eventOrder;
		for(const AstAttribute& attribute : command.attributes)
			event.attrs[attribute.name] = attribute.value;

		// --- picture -----------------------------------------------------------
		if(name == "bg")
		{
			std::string error;
			if(ApplyBackground(command, error))
				m_stats.handled["bg"]++;
			return;
		}
		if(name == "cgdel")
		{
			// deleting a layer draws nothing, so it is safe even for the layers this
			// port never *fills*: the ids are the only thing touched
			const int id = IntAttr(command.attributes, "id", -1);
			if(m_config.stage != nullptr && id >= 0)
			{
				m_config.stage->DeleteLayer(std::to_string(id));
				m_stats.stageOps++;
			}
			m_stats.handled["cgdel"]++;
			return;
		}
		if(name == "text")
		{
			m_stats.handled["text"]++;
			return;
		}

		// --- sound -------------------------------------------------------------
		if(name == "se" || name == "seplay")
		{
			event.kind = SceneEvent::Kind::Audio;
			event.file = ResolveAstAsset(event.Str("path", "se/"), event.Str("file"));
			m_events.push_back(event);
			m_stats.handled[name]++;
			return;
		}
		if(name == "vo" || name == "lvo")
		{
			event.kind = SceneEvent::Kind::Audio;
			event.file = ResolveAstAsset(event.Str("path", name == "lvo" ? "bgv/" : "vo/"),
				event.Str("file"));
			m_events.push_back(event);
			m_stats.handled[name]++;
			return;
		}
		if(name == "bgm" || name == "chmbgm" || name == "mbgm")
		{
			event.kind = SceneEvent::Kind::Audio;
			std::string file = event.Str("file");
			if(name == "chmbgm" && event.Has("ch"))
				file = event.Str("ch");
			event.file = ResolveAstAsset(event.Str("path", "bgm/"), file);
			m_events.push_back(event);
			m_stats.handled[name]++;
			return;
		}
		if(name == "bgtstop" || name == "mbgmstop")
		{
			event.kind = SceneEvent::Kind::Audio;
			m_events.push_back(event);
			m_stats.handled[name]++;
			return;
		}

		// --- the picture moves -------------------------------------------------
		if(name == "ex")
		{
			event.kind = SceneEvent::Kind::Wait;
			m_events.push_back(event);
			m_stats.handled["ex"]++;
			return;
		}
		if(name == "quake" || name == "flash" || name == "extrans" || name == "colortone")
		{
			event.kind = SceneEvent::Kind::Effect;
			m_events.push_back(event);
			m_stats.handled[name]++;
			return;
		}
		if(name == "select")
		{
			event.kind = SceneEvent::Kind::Input;
			m_events.push_back(event);
			m_stats.handled["select"]++;
			return;
		}

		// --- character and event art: refused, counted, never read -------------
		if(name == "fg" || name == "fgact" || name == "fgf" || name == "fgfact" ||
			name == "cgact" || name == "evset" || name == "scene")
		{
			m_stats.policySkipped++;
			m_stats.policy[name]++;
			return;
		}

		// --- script state this runtime can follow ------------------------------
		if(name == "eval")
		{
			// `exp="g.d01_c05_1=1"`: the only shape the corpus has
			const std::string expression = event.Str("exp");
			const size_t equals = expression.find('=');
			if(equals != std::string::npos)
			{
				const std::string key = expression.substr(0, equals);
				const std::string value = expression.substr(equals + 1);
				// carried out as an event, not applied here: a front end that models
				// variables is the one that decides what a variable is
				event.kind = SceneEvent::Kind::Other;
				event.attrs["__key"] = key;
				event.attrs["__value"] = value;
				m_events.push_back(event);
				m_stats.handled["eval"]++;
				return;
			}
		}
		if(name == "savetitle" || name == "cacheclear" || name == "exfont" ||
			name == "skipstop" || name == "autoplay")
		{
			m_stats.bookkeeping++;
			m_stats.handled[name]++;
			return;
		}

		// --- everything else: counted, located, reported -----------------------
		m_stats.unwired[name]++;
		if(m_stats.unwiredAt.find(name) == m_stats.unwiredAt.end())
			m_stats.unwiredAt[name] = BlockName(block, blockIndex) + ":" +
				std::to_string(block.line) + " (command " + std::to_string(index) + ")";
	}

	bool AstRun::Step(PageText& page, bool& hasPage, std::string& error)
	{
		hasPage = false;
		error.clear();
		if(!m_error.empty())
		{
			error = m_error;
			return false;
		}
		if(AtEnd())
			return false;
		if(m_config.maxPages > 0 && m_blocksWithPage >= m_config.maxPages)
		{
			m_cursor = m_chain.order.size();
			return false;
		}

		const int blockIndex = m_chain.order[m_cursor];
		const AstBlock& block = m_ast.blocks[blockIndex];
		m_stats.blocksWalked++;
		for(size_t i = 0; i < block.commands.size(); i++)
		{
			m_stats.commands++;
			ApplyCommand(block.commands[i], block, (int)m_cursor, (int)i);
			m_eventOrder++;
		}

		// the words through the source, so an external `lang_NNNNN` layout is a
		// different source and not a different player
		const std::vector<AstLine> lines = m_config.lines != nullptr
			? m_config.lines->LinesFor(block) : block.lines;
		if(!lines.empty())
		{
			page = PageText();
			for(size_t i = 0; i < lines.size(); i++)
			{
				if(i > 0)
					page.text += "\n";
				page.text += lines[i].text;
				if(page.speaker.empty())
					page.speaker = lines[i].speaker;
			}
			hasPage = true;
			m_stats.pages++;
			m_blocksWithPage++;
		}

		m_cursor++;
		if(m_config.maxBlocks > 0 && (int)m_cursor >= m_config.maxBlocks)
			m_cursor = m_chain.order.size();
		return true;
	}
}
