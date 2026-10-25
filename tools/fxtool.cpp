// fxtool.cpp - the effects layer, exercised outside a window
//
// `agame` counts the animation tags and moves on, because a still frame has no
// time axis.  This tool gives that axis to the five things that need one -
// typewriter reveal, tweened values, anime frames, sfade brightness, quake
// offset - over the *real* files, and proves each of them with an assertion
// that can be broken on purpose (see the "mutation spot" notes in the selftest).
//
//   fxtool selftest
//   fxtool pages   <real.iet> [--limit N]
//   fxtool typewriter <real.iet> --page N --at 0,50,100 [--msgspeed 30]
//   fxtool tween  --from 0 --to 255 --ms 1000 --at 0,250,500,1000
//                 [--ease easeout_quad] [--delay 0] [--yoyo 0]
//   fxtool scetween --base 200 --diff -255 --ms 400 [--ease ...] --at 0,100,200
//   fxtool lytween <real.iet>            every [lytween], and the broken ones
//   fxtool anime  <line1.ipt> --at 0,100,250,400        frame of the descriptor
//   fxtool anime  <real.iet> --id 82 --at 0,400,900,3600 keyframe track of the tag
//   fxtool sfade  <in.png> <out.png> --gain 500 [--time 2000 --at 1000]
//   fxtool quake  --seed 7 --size 30 --count 2 --ms 300 --dir h --at 0,100
//
// The real-data commands SKIP (not fail) when testdata is absent: the build must
// stay usable in a tree without the corpus.

#include "fx/animation.h"
#include "fx/brightness.h"
#include "fx/pages.h"
#include "fx/quake.h"
#include "fx/tween.h"
#include "fx/typewriter.h"
#include "fx/colortone.h"
#include "fx/extrans.h"
#include "fx/excall.h"
#include "fx/guistate.h"
#include "fx/tags.h"

#include "iet.h"
#include "lua.h"
#include "luaast.h"
#include "ipt.h"
#include "pngio.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <unistd.h>
#endif

namespace fx = artemis::fx;

namespace
{
	// ---------------------------------------------------------------------------
	// option parsing
	// ---------------------------------------------------------------------------
	struct Options
	{
		std::string ease = "none";
		std::string id;
		long long from = 0; // --from / --base / --size / --gain
		long long to = 0;   // --to / --diff
		long long ms = 0;   // --ms / --time
		long long delay = 0;
		int yoyo = 0;
		unsigned long long seed = 0;
		int page = -1;
		int count = 1;
		int msgspeed = 30;
		bool vertical = false;
		std::string style = "sepia";
		bool bench = false;
		std::vector<long long> at;
	};

	// ---------------------------------------------------------------------------
	// test harness
	// ---------------------------------------------------------------------------
	struct Test
	{
		int passed = 0;
		int failed = 0;
		int skipped = 0;
		std::vector<std::string> failures;
	};

	Test g_test;

	void Pass(const char* name, const std::string& detail = std::string())
	{
		g_test.passed++;
		if(detail.empty())
			printf("PASS %-30s\n", name);
		else
			printf("PASS %-30s %s\n", name, detail.c_str());
	}

	void Skip(const char* name, const std::string& why)
	{
		g_test.skipped++;
		printf("SKIP %-30s %s\n", name, why.c_str());
	}

	bool Check(const char* name, bool ok, const std::string& detail)
	{
		if(ok)
			Pass(name, detail);
		else
		{
			g_test.failed++;
			g_test.failures.push_back(std::string(name) + ": " + detail);
			printf("FAIL %-30s %s\n", name, detail.c_str());
		}
		return ok;
	}

	bool CheckEq(const char* name, long long got, long long want)
	{
		char detail[192];
		snprintf(detail, sizeof(detail), "got %lld want %lld", got, want);
		return Check(name, got == want, detail);
	}

	bool CheckStr(const char* name, const std::string& got, const std::string& want)
	{
		if(got == want)
			return Check(name, true, "\"" + got + "\"");
		return Check(name, false, "got \"" + got + "\" want \"" + want + "\"");
	}

	// ---------------------------------------------------------------------------
	// small helpers
	// ---------------------------------------------------------------------------
	bool ReadWholeFile(const std::string& path, std::string& out)
	{
		FILE* file = fopen(path.c_str(), "rb");
		if(file == nullptr)
			return false;
		char buffer[65536];
		size_t got = 0;
		while((got = fread(buffer, 1, sizeof(buffer), file)) > 0)
			out.append(buffer, got);
		fclose(file);
		return true;
	}

	bool IsFile(const std::string& path)
	{
		FILE* file = fopen(path.c_str(), "rb");
		if(file == nullptr)
			return false;
		fclose(file);
		return true;
	}

	bool HasSuffix(const std::string& text, const std::string& suffix)
	{
		return text.size() >= suffix.size() &&
			text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
	}

	std::string ExecutableDirectory()
	{
#ifdef __APPLE__
		char buffer[4096];
		uint32_t size = sizeof(buffer);
		if(_NSGetExecutablePath(buffer, &size) == 0)
		{
			char resolved[4096];
			if(realpath(buffer, resolved) != nullptr)
			{
				const std::string path = resolved;
				const size_t slash = path.find_last_of('/');
				return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
			}
		}
#endif
		return std::string(".");
	}

	// Where the corpus lives.  Same idea as the other tools, except that this one
	// also has to work when CMake did not define ARTEMIS_TESTDATA: fxtool is
	// created by a glob and should not need a new line in CMakeLists to find data.
	std::string TestDataRoot()
	{
		const char* env = getenv("FXT_TESTDATA");
		if(env != nullptr &&
			IsFile(std::string(env) + "/artemis-nicolan/extracted/scenario/scene.iet"))
			return env;
#ifdef ARTEMIS_TESTDATA
		if(IsFile(std::string(ARTEMIS_TESTDATA) +
			"/artemis-nicolan/extracted/scenario/scene.iet"))
			return ARTEMIS_TESTDATA;
#endif
		const char* relative[] = { "../testdata", "../../testdata", "testdata", "." };
		for(const char* candidate : relative)
			if(IsFile(std::string(candidate) +
				"/artemis-nicolan/extracted/scenario/scene.iet"))
				return candidate;
		const std::string exe = ExecutableDirectory();
		const char* up[] = { "..", "../..", "../../..", "../../../.." };
		for(const char* level : up)
		{
			const std::string base = exe + "/" + level;
			for(const char* candidate : relative)
			{
				const std::string root = base + "/" + candidate;
				if(IsFile(root + "/artemis-nicolan/extracted/scenario/scene.iet"))
					return root;
			}
		}
		return std::string();
	}

	std::string Trim(const std::string& text)
	{
		size_t first = 0;
		while(first < text.size() && (text[first] == ' ' || text[first] == '\t'))
			first++;
		size_t last = text.size();
		while(last > first && (text[last - 1] == ' ' || text[last - 1] == '\t'))
			last--;
		return text.substr(first, last - first);
	}

	std::vector<std::string> SplitCommas(const std::string& text)
	{
		std::vector<std::string> out;
		size_t at = 0;
		while(at <= text.size())
		{
			const size_t comma = text.find(',', at);
			const size_t end = comma == std::string::npos ? text.size() : comma;
			const std::string part = Trim(text.substr(at, end - at));
			if(!part.empty())
				out.push_back(part);
			if(comma == std::string::npos)
				break;
			at = comma + 1;
		}
		return out;
	}

	bool ParseTimes(const std::string& text, std::vector<long long>& out)
	{
		for(const std::string& part : SplitCommas(text))
		{
			char* end = nullptr;
			const long long value = strtoll(part.c_str(), &end, 10);
			if(end == nullptr || *end != '\0')
				return false;
			out.push_back(value);
		}
		return true;
	}

	// A one-line rendering of page text: '/' is the page's own line break.
	std::string OneLine(const std::string& text)
	{
		std::string out;
		for(char c : text)
			out += (c == '\n') ? '/' : c;
		return out;
	}

	// A quoted rendering of a revealed prefix; a cut inside a character would
	// show up here as a stray byte, next to the assertions that forbid it.
	std::string Quad(const std::string& text)
	{
		std::string out = "\"";
		for(char c : text)
		{
			if(c == '\n')
			{
				out += "\\n";
				continue;
			}
			out += c;
		}
		return out + "\"";
	}

	// ---------------------------------------------------------------------------
	// the real corpus
	// ---------------------------------------------------------------------------
	struct Corpus
	{
		std::string root;
		std::string scene;      // scenario/scene.iet, Shift-JIS
		std::string first;      // first.iet, Shift-JIS, the GUI script
		std::string animeIpt;   // samples/ipt/image_hd_anime_line1.ipt
		std::string background; // bgimage/main.png, 1024x698 = the stage size
		std::string astDir;     // henpri trial: 59 .ast scripts (colortone/excall/...)
		std::string macroEffect; // nicolan system/macro_effect.iet (msec= spelling)

		bool ok() const { return !scene.empty(); }
	};

	Corpus FindCorpus()
	{
		Corpus corpus;
		corpus.root = TestDataRoot();
		if(corpus.root.empty())
			return corpus;
		corpus.scene = corpus.root + "/artemis-nicolan/extracted/scenario/scene.iet";
		corpus.first = corpus.root + "/artemis-nicolan/extracted/first.iet";
		corpus.animeIpt =
			corpus.root + "/artemis-raspberry-web/samples/ipt/image_hd_anime_line1.ipt";
		corpus.background = corpus.root + "/artemis-nicolan/extracted/bgimage/main.png";
		corpus.astDir = corpus.root + "/artemis-henpri-trial/extracted/script";
		corpus.macroEffect =
			corpus.root + "/artemis-nicolan/extracted/system/macro_effect.iet";
		if(!IsFile(corpus.background))
			corpus.background.clear();
		if(!std::filesystem::is_directory(corpus.astDir))
			corpus.astDir.clear();
		if(!IsFile(corpus.macroEffect))
			corpus.macroEffect.clear();
		if(!IsFile(corpus.first))
			corpus.first.clear();
		if(!IsFile(corpus.animeIpt))
			corpus.animeIpt.clear();
		if(!IsFile(corpus.background))
			corpus.background.clear();
		return corpus;
	}

	bool LoadScript(const std::string& path, artemis::IetScript& script, std::string& error)
	{
		std::string bytes;
		if(!ReadWholeFile(path, bytes))
		{
			error = "cannot read " + path;
			return false;
		}
		const std::string decoded = artemis::DecodeScenarioText(
			(const uint8_t*)bytes.data(), bytes.size());
		if(decoded.empty())
		{
			error = path + ": not decodable as Shift-JIS or UTF-8";
			return false;
		}
		if(!artemis::IetScript::Parse(decoded, script, error))
			return false;
		script.SetName(path);
		return true;
	}

	std::vector<fx::FeedItem> ToFeed(const artemis::IetScript& script)
	{
		std::vector<fx::FeedItem> out;
		out.reserve(script.items().size());
		for(const artemis::Item& item : script.items())
		{
			if(item.kind == artemis::Item::Kind::Text)
				out.push_back(fx::FeedItem::Text_(item.text, item.line));
			else if(item.kind == artemis::Item::Kind::Command)
				out.push_back(fx::FeedItem::Command_(item.text, item.line,
					item.AttrOr("data", std::string())));
		}
		return out;
	}

	// Every page of a script, with the glyph count its typewriter would take.
	struct PagedScript
	{
		std::vector<fx::Page> pages;
		std::vector<fx::Text> texts;
		fx::PageFeedStats stats;
		int atSigns = 0;    // '@' inside text items, after decoding
		int rawAtSigns = 0; // '@' bytes in the Shift-JIS file: not the same number
		size_t glyphs = 0;
	};

	bool PageScript(const std::string& path, PagedScript& out, std::string& error)
	{
		artemis::IetScript script;
		if(!LoadScript(path, script, error))
			return false;
		const std::vector<fx::FeedItem> feed = ToFeed(script);
		out.pages = fx::SplitPages(feed, &out.stats);
		for(const fx::FeedItem& item : feed)
		{
			if(item.kind != fx::FeedItem::Kind::Text)
				continue;
			for(char c : item.text)
				if(c == '@')
					out.atSigns++;
		}
		out.texts.reserve(out.pages.size());
		for(const fx::Page& page : out.pages)
		{
			out.texts.push_back(fx::Segment(page.text));
			out.glyphs += out.texts.back().timedGlyphs;
		}
		std::string bytes;
		if(ReadWholeFile(path, bytes))
			for(char c : bytes)
				if(c == '@')
					out.rawAtSigns++;
		return true;
	}


	// ---------------------------------------------------------------------------
	// real .ast data: the tag census and the cross-script loader
	// ---------------------------------------------------------------------------
	std::vector<fx::AttrNameValue> ToFxAttrs(const std::vector<artemis::TagAttr>& attrs)
	{
		std::vector<fx::AttrNameValue> out;
		out.reserve(attrs.size());
		for(const artemis::TagAttr& attr : attrs)
			out.push_back(fx::AttrNameValue{ attr.name, attr.value });
		return out;
	}

	std::vector<fx::AttrNameValue> ToFxAttrs(const std::vector<artemis::AstAttribute>& attrs)
	{
		std::vector<fx::AttrNameValue> out;
		out.reserve(attrs.size());
		for(const artemis::AstAttribute& attr : attrs)
			out.push_back(fx::AttrNameValue{ attr.name, attr.value });
		return out;
	}

	std::string FxAttrOf(const std::vector<fx::AttrNameValue>& attrs,
		const std::string& name)
	{
		for(const fx::AttrNameValue& attr : attrs)
			if(attr.name == name)
				return attr.value;
		return std::string();
	}

	struct AstCorpusStats
	{
		int files = 0;
		int parseFailures = 0;
		int labelsTop = 0;
		int labelsOther = 0;
		int colortone = 0, colortoneReset = 0, colortoneSepia = 0, colortoneUnknown = 0;
		int fgSepia = 0;
		int extrans = 0, extransNoTime = 0, extransSync0 = 0;
		int excall = 0, excallWithLabel = 0;
		int movie = 0, movieCancel = 0, movieBlackBg = 0;
		int titleTag = 0, autoplay = 0, autoplayStop = 0, skipstop = 0;
		int cacheclear = 0, exfont = 0, exfontSized = 0, exfontColour = 0;
		int inlineExfont = 0, delayCommands = 0;
		int savetitle = 0;
		std::map<std::string, int> tagCounts;
		std::set<std::string> excallTargets;
		std::set<std::string> savetitleTexts;
		std::vector<std::string> savetitleAll;
		std::map<std::string, int> exfontSizes;
		std::map<std::string, int> exfontColours;
		std::vector<fx::MovieTag> movies;
		std::vector<int64_t> extransTimes;
		std::map<int64_t, int> extransTimeCounts;
	};

	bool ScanAstCorpus(const std::string& directory, AstCorpusStats& out,
		std::string& error)
	{
		out = AstCorpusStats{};
		std::error_code ec;
		if(directory.empty() || !std::filesystem::is_directory(directory, ec))
		{
			error = "no .ast corpus at " + directory;
			return false;
		}
		std::vector<std::string> paths;
		for(std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end;
			it.increment(ec))
		{
			if(it->is_regular_file() && it->path().extension() == ".ast")
				paths.push_back(it->path().string());
		}
		std::sort(paths.begin(), paths.end());

		// One census for a command, whether it sits in a block, in a `delay`
		// schedule, or (for inline tags) inside a dialogue line.
		auto census = [&out](const std::string& name,
			const std::vector<fx::AttrNameValue>& attrs) {
			out.tagCounts[name]++;
			if(name == "colortone")
			{
				fx::ColorToneTag tag;
				fx::ParseColorToneTag(attrs, tag);
				out.colortone++;
				if(tag.valid && tag.value == fx::ColorStyle::Reset)
					out.colortoneReset++;
				else if(tag.valid && tag.value == fx::ColorStyle::Sepia)
					out.colortoneSepia++;
				else
					out.colortoneUnknown++;
			}
			else if(name == "fg" && FxAttrOf(attrs, "style") == "sepia")
				out.fgSepia++;
			else if(name == "extrans")
			{
				fx::ExtransTag tag;
				std::string note;
				if(fx::ParseExtransTag(attrs, tag, note))
				{
					out.extrans++;
					if(!tag.hasTime)
						out.extransNoTime++;
					if(tag.hasSync && tag.sync == 0)
						out.extransSync0++;
					out.extransTimes.push_back(tag.timeMs);
					out.extransTimeCounts[tag.timeMs]++;
				}
			}
			else if(name == "excall")
			{
				out.excall++;
				const std::string target = FxAttrOf(attrs, "file");
				if(!target.empty())
					out.excallTargets.insert(target);
				if(!FxAttrOf(attrs, "label").empty())
					out.excallWithLabel++;
			}
			else if(name == "movie")
			{
				fx::MovieTag tag;
				std::string note;
				if(fx::ParseMovieTag(attrs, tag, note))
				{
					out.movie++;
					if(tag.cancel)
						out.movieCancel++;
					if(tag.bg == "black")
						out.movieBlackBg++;
					out.movies.push_back(tag);
				}
			}
			else if(name == "タイトル")
				out.titleTag++;
			else if(name == "autoplay")
			{
				fx::AutoplayTag tag;
				std::string note;
				fx::ParseAutoplayTag(attrs, tag, note);
				out.autoplay++;
				if(tag.stop)
					out.autoplayStop++;
			}
			else if(name == "skipstop")
				out.skipstop++;
			else if(name == "cacheclear")
				out.cacheclear++;
			else if(name == "exfont")
			{
				fx::ExfontTag tag;
				std::string note;
				fx::ParseExfontTag(attrs, tag, note);
				out.exfont++;
				if(tag.hasSize)
				{
					out.exfontSized++;
					out.exfontSizes[tag.size]++;
				}
				const std::string colour = FxAttrOf(attrs, "colour");
				const std::string color = colour.empty() ? FxAttrOf(attrs, "color") : colour;
				if(!color.empty())
				{
					out.exfontColour++;
					out.exfontColours[color]++;
				}
			}
			else if(name == "savetitle")
			{
				fx::SaveTitleTag tag;
				std::string note;
				if(fx::ParseSaveTitleTag(attrs, tag, note))
				{
					out.savetitle++;
					out.savetitleTexts.insert(tag.text);
					out.savetitleAll.push_back(tag.text);
				}
			}
		};

		auto luaAttrs = [](const artemis::LuaValue& command) {
			std::vector<fx::AttrNameValue> attrs;
			for(const auto& entry : command.entries)
				attrs.push_back(fx::AttrNameValue{ entry.first, entry.second.text });
			return attrs;
		};

		for(const std::string& path : paths)
		{
			std::string text;
			if(!ReadWholeFile(path, text))
			{
				out.parseFailures++;
				continue;
			}
			artemis::LuaAst ast;
			std::string parseError;
			if(!artemis::ParseLuaAst(text, ast, parseError))
			{
				out.parseFailures++;
				continue;
			}
			out.files++;
			for(const artemis::AstBlock& block : ast.blocks)
				for(const artemis::AstCommand& command : block.commands)
					census(command.name, ToFxAttrs(command.attributes));

			// The block-level parser stops at the block: `delay` schedules and
			// inline text tags ({"exfont"}) are nested tables it does not enter.
			// The census has to, or the corpus count would be off by 21 fg tags
			// and all 266 exfont tags.
			artemis::LuaValue root;
			if(!artemis::ParseLuaTable(text, root, parseError))
				continue;
			const artemis::LuaValue* astObject = root.field("ast");
			if(astObject == nullptr || !astObject->isObject())
				continue;
			const artemis::LuaValue* labels = astObject->field("label");
			if(labels != nullptr && labels->isObject())
				for(const auto& entry : labels->entries)
				{
					if(entry.first == "top")
						out.labelsTop++;
					else
						out.labelsOther++;
				}
			for(const auto& blockEntry : astObject->entries)
			{
				if(blockEntry.first.rfind("block_", 0) != 0 || !blockEntry.second.isObject())
					continue;
				const artemis::LuaValue* delay = blockEntry.second.field("delay");
				if(delay != nullptr && delay->isObject())
					for(const auto& slot : delay->entries)
					{
						if(!slot.second.isObject())
							continue;
						for(const artemis::LuaValue& value : slot.second.items)
						{
							if(!value.isObject() || value.items.empty()
								|| value.items[0].kind != artemis::LuaValue::Kind::String)
								continue;
							census(value.items[0].text, luaAttrs(value));
							out.delayCommands++;
						}
					}
				const artemis::LuaValue* textTable = blockEntry.second.field("text");
				if(textTable == nullptr || !textTable->isObject())
					continue;
				auto walkInline = [&](const artemis::LuaValue& value, auto&& self) -> void {
					if(!value.isObject())
						return;
					for(const artemis::LuaValue& item : value.items)
					{
						if(!item.isObject())
							continue;
						if(!item.items.empty()
							&& item.items[0].kind == artemis::LuaValue::Kind::String
							&& item.items[0].text == "exfont")
						{
							census("exfont", luaAttrs(item));
							out.inlineExfont++;
						}
						self(item, self);
					}
					for(const auto& entry : value.entries)
						self(entry.second, self);
				};
				walkInline(*textTable, walkInline);
			}
		}
		return true;
	}

	bool LoadAstProgram(const std::string& directory, const std::string& file,
		fx::ScriptProgram& out, std::string& error)
	{
		const std::string path = directory + "/" + file + ".ast";
		std::string text;
		if(!ReadWholeFile(path, text))
		{
			error = "not in this corpus (" + path + ")";
			return false;
		}
		artemis::LuaAst ast;
		if(!artemis::ParseLuaAst(text, ast, error))
			return false;
		out = fx::ScriptProgram{};
		out.file = file;
		for(const artemis::AstBlock& block : ast.blocks)
		{
			fx::ScriptBlock scriptBlock;
			scriptBlock.name = block.name;
			scriptBlock.next = block.next;
			for(const artemis::AstCommand& command : block.commands)
			{
				fx::ScriptCommand scriptCommand;
				scriptCommand.name = command.name;
				scriptCommand.attrs = ToFxAttrs(command.attributes);
				scriptBlock.commands.push_back(std::move(scriptCommand));
			}
			out.blocks.push_back(std::move(scriptBlock));
		}
		artemis::LuaValue root;
		std::string labelError;
		if(artemis::ParseLuaTable(text, root, labelError))
		{
			const artemis::LuaValue* astObject = root.field("ast");
			const artemis::LuaValue* labels = astObject ? astObject->field("label") : nullptr;
			if(labels != nullptr && labels->isObject())
				for(const auto& entry : labels->entries)
				{
					if(!entry.second.isObject())
						continue;
					const artemis::LuaValue* block = entry.second.field("block");
					if(block != nullptr && block->kind == artemis::LuaValue::Kind::String)
						out.labels.push_back({ entry.first, block->text });
				}
		}
		return true;
	}

	struct GuiScriptStats
	{
		int total = 0, init = 0, add = 0, other = 0;
		int msecSpelling = 0, timeSpelling = 0, commented = 0;
		std::map<std::string, int> types;
		std::map<std::string, int> signatureCounts;
		std::vector<fx::GuiDefinition> definitions;
	};

	bool ScanGuiScript(const std::string& path, GuiScriptStats& out, std::string& error)
	{
		out = GuiScriptStats{};
		artemis::IetScript script;
		if(!LoadScript(path, script, error))
			return false;
		for(const artemis::Item& item : script.items())
		{
			if(item.kind != artemis::Item::Kind::Command || item.text != "scetween")
				continue;
			fx::GuiDefinition definition;
			definition.mode = item.AttrOr("mode", "");
			definition.type = item.AttrOr("type", item.AttrOr("id", ""));
			definition.param = item.AttrOr("param", "");
			const std::string diff = item.AttrOr("diff", "");
			if(!diff.empty())
			{
				definition.diff = atoll(diff.c_str());
				definition.hasDiff = true;
			}
			std::string time = item.AttrOr("time", "");
			if(!time.empty())
				definition.timeSpelling = "time";
			else
			{
				time = item.AttrOr("msec", "");
				if(!time.empty())
					definition.timeSpelling = "msec";
			}
			if(!time.empty())
				definition.timeMs = atoll(time.c_str());
			definition.delay = item.AttrOr("delay", "");
			definition.ease = item.AttrOr("ease", "");
			out.definitions.push_back(definition);
			out.total++;
			if(definition.mode == "init")
				out.init++;
			else if(definition.mode == "add")
				out.add++;
			else
				out.other++;
			out.types[definition.type]++;
			if(definition.timeSpelling == "msec")
				out.msecSpelling++;
			else if(definition.timeSpelling == "time")
				out.timeSpelling++;
			char signature[256];
			snprintf(signature, sizeof(signature), "%s|%s|%s|%s",
				definition.type.c_str(), definition.mode.c_str(),
				definition.param.c_str(), diff.c_str());
			out.signatureCounts[signature]++;
		}
		// commented definitions are not in the item stream; count them in the text
		std::string bytes;
		if(ReadWholeFile(path, bytes))
		{
			const std::string decoded = artemis::DecodeScenarioText(
				(const uint8_t*)bytes.data(), bytes.size());
			size_t at = 0;
			while(at < decoded.size())
			{
				const size_t end = decoded.find('\n', at);
				const std::string line = decoded.substr(at,
					end == std::string::npos ? std::string::npos : end - at);
				const size_t first = line.find_first_not_of(" \t\r");
				if(first != std::string::npos && line.compare(first, 2, "//") == 0
					&& line.find("[scetween", first) != std::string::npos)
					out.commented++;
				if(end == std::string::npos)
					break;
				at = end + 1;
			}
		}
		return true;
	}

	// The [anime] keyframe track of one layer id, read from the script as it is.
	struct TrackReport
	{
		fx::AnimeTrack track;
		int initTags = 0;
		int addTags = 0;
		int endTags = 0;
	};

	void BuildTrack(const artemis::IetScript& script, const std::string& id, TrackReport& out)
	{
		for(const artemis::Item& item : script.items())
		{
			if(item.kind != artemis::Item::Kind::Command || item.text != "anime")
				continue;
			if(item.AttrOr("id", std::string()) != id)
				continue;
			const std::string mode = item.AttrOr("mode", std::string("add"));
			if(mode == "end")
			{
				out.endTags++;
				out.track.endHoldMs = atoll(item.AttrOr("time", "0").c_str());
				continue;
			}
			fx::AnimeKey key;
			const artemis::TagAttr* file = item.Attr("file");
			if(file != nullptr)
			{
				key.file = file->value;
				key.hasFile = true;
			}
			const artemis::TagAttr* top = item.Attr("top");
			if(top != nullptr)
			{
				key.top = atoi(top->value.c_str());
				key.hasTop = true;
			}
			const artemis::TagAttr* left = item.Attr("left");
			if(left != nullptr)
			{
				key.left = atoi(left->value.c_str());
				key.hasLeft = true;
			}
			const artemis::TagAttr* alpha = item.Attr("alpha");
			if(alpha != nullptr)
			{
				key.alpha = atoi(alpha->value.c_str());
				key.hasAlpha = true;
			}
			key.arriveMs = atoll(item.AttrOr("time", "0").c_str());
			if(mode == "init")
			{
				out.initTags++;
				if(out.track.keys.empty())
					out.track.keys.push_back(key);
				else
					out.track.keys[0] = key;
			}
			else
			{
				out.addTags++;
				out.track.keys.push_back(key);
			}
		}
	}

	// Every [lytween] in a script, parsed: the corpus contains two broken ones
	// and the point of this report is that they are refused *loudly*.
	struct LytweenReport
	{
		int tags = 0;
		int usable = 0;
		int refused = 0;
		int yoyoForever = 0;
		int yoyoFinite = 0;
		std::vector<fx::LytweenTag> parsed;
		std::vector<std::string> notes;
	};

	void CollectLytween(const artemis::IetScript& script, LytweenReport& out)
	{
		for(const artemis::Item& item : script.items())
		{
			if(item.kind != artemis::Item::Kind::Command || item.text != "lytween")
				continue;
			out.tags++;
			std::vector<fx::AttrNameValue> attrs;
			for(const artemis::TagAttr& attr : item.attrs)
			{
				fx::AttrNameValue copy;
				copy.name = attr.name;
				copy.value = attr.value;
				attrs.push_back(copy);
			}
			fx::LytweenTag tag;
			if(fx::ParseLytween(attrs, tag))
			{
				out.usable++;
				if(tag.tween.yoyo < 0)
					out.yoyoForever++;
				else if(tag.tween.yoyo > 0)
					out.yoyoFinite++;
				out.parsed.push_back(tag);
			}
			else
			{
				out.refused++;
				out.notes.push_back("line " + std::to_string(item.line) + " id=\"" +
					item.AttrOr("id", std::string()) + "\": " + tag.note);
			}
		}
	}

	// Pixel statistics: what actually changed on screen.
	struct PixelStats
	{
		int width = 0;
		int height = 0;
		double meanR = 0, meanG = 0, meanB = 0, meanA = 0;
		int minR = 255, maxR = 0;
		size_t distinct = 0;
	};

	PixelStats Measure(const artemis::Image& image)
	{
		PixelStats stats;
		stats.width = image.width;
		stats.height = image.height;
		const size_t pixels = image.pixels();
		if(pixels == 0)
			return stats;
		double sumR = 0, sumG = 0, sumB = 0, sumA = 0;
		for(size_t i = 0; i < pixels; i++)
		{
			const uint8_t* px = &image.rgba[i * 4];
			sumR += px[0];
			sumG += px[1];
			sumB += px[2];
			sumA += px[3];
			if((int)px[0] < stats.minR)
				stats.minR = (int)px[0];
			if((int)px[0] > stats.maxR)
				stats.maxR = (int)px[0];
		}
		stats.meanR = sumR / (double)pixels;
		stats.meanG = sumG / (double)pixels;
		stats.meanB = sumB / (double)pixels;
		stats.meanA = sumA / (double)pixels;
		stats.distinct = artemis::CountColors(image.rgba.data(), pixels);
		return stats;
	}

	// ---------------------------------------------------------------------------
	// commands
	// ---------------------------------------------------------------------------
	int CommandPages(const std::string& path, int limit)
	{
		PagedScript paged;
		std::string error;
		if(!PageScript(path, paged, error))
		{
			fprintf(stderr, "fxtool: %s\n", error.c_str());
			return 1;
		}
		printf("%s\n", path.c_str());
		printf("  pages %d, empty [rp] dropped %d, inline markup dropped %d, "
			"other commands %d\n", paged.stats.pages, paged.stats.emptyDropped,
			paged.stats.inlineDropped, paged.stats.otherCommands);
		printf("  timed glyphs %zu, '@' breaks in text %d (raw '@' bytes in the file %d)\n",
			paged.glyphs, paged.atSigns, paged.rawAtSigns);
		const int total = (int)paged.pages.size();
		const int count = limit > 0 && limit < total ? limit : total;
		for(int i = 0; i < count; i++)
		{
			const fx::Page& page = paged.pages[(size_t)i];
			printf("  [%3d] line %-5d glyphs %-4zu %s\n", i, page.line,
				paged.texts[(size_t)i].timedGlyphs, OneLine(page.text).c_str());
		}
		return 0;
	}

	int CommandTypewriter(const std::string& path, const Options& options)
	{
		PagedScript paged;
		std::string error;
		if(!PageScript(path, paged, error))
		{
			fprintf(stderr, "fxtool: %s\n", error.c_str());
			return 1;
		}
		if(options.page < 0 || options.page >= (int)paged.pages.size())
		{
			fprintf(stderr, "fxtool: page %d out of range (0..%d)\n", options.page,
				(int)paged.pages.size() - 1);
			return 2;
		}
		const fx::Page& page = paged.pages[(size_t)options.page];
		const fx::Text& text = paged.texts[(size_t)options.page];
		printf("%s page %d (source line %d)\n", path.c_str(), options.page, page.line);
		printf("  text   %s\n", OneLine(page.text).c_str());
		printf("  glyphs %zu timed, %zu bytes, complete at %lld ms @ %d ms/glyph\n",
			text.timedGlyphs, text.utf8.size(),
			(long long)fx::RevealDurationMs(text, options.msgspeed), options.msgspeed);
		std::vector<long long> times = options.at;
		if(times.empty())
			times = { 0, 50, 100 };
		for(long long at : times)
		{
			const fx::Reveal reveal = fx::RevealAt(text, at, options.msgspeed);
			printf("  t=%-6lld glyphs %-4zu bytes %-4zu %-9s %s\n", at, reveal.glyphs,
				reveal.bytes, reveal.complete ? "complete" : "revealing",
				Quad(reveal.shown).c_str());
		}
		return 0;
	}

	int CommandTween(const Options& options)
	{
		fx::Tween tween;
		tween.from = options.from;
		tween.to = options.to;
		tween.durationMs = options.ms;
		tween.delayMs = options.delay;
		tween.yoyo = options.yoyo;
		if(!fx::ParseEase(options.ease, tween.ease))
		{
			fprintf(stderr, "fxtool: unknown ease \"%s\"\n", options.ease.c_str());
			return 2;
		}
		printf("tween %lld -> %lld over %lld ms, ease %s, delay %lld, yoyo %d\n",
			(long long)tween.from, (long long)tween.to, (long long)tween.durationMs,
			fx::EaseName(tween.ease), (long long)tween.delayMs, tween.yoyo);
		const long long total = tween.totalMs();
		if(total < 0)
			printf("  never stops (yoyo < 0)\n");
		else
			printf("  stops at %lld ms\n", total);
		std::vector<long long> times = options.at;
		if(times.empty())
			times = { 0, 250, 500, 1000 };
		for(long long at : times)
			printf("  t=%-6lld %lld\n", at, (long long)fx::ValueAt(tween, at));
		return 0;
	}

	int CommandScetween(const Options& options)
	{
		fx::Scetween scetween;
		scetween.diff = options.to;
		scetween.durationMs = options.ms > 0 ? options.ms : 400;
		scetween.delayMs = options.delay;
		if(!fx::ParseEase(options.ease, scetween.ease))
		{
			fprintf(stderr, "fxtool: unknown ease \"%s\"\n", options.ease.c_str());
			return 2;
		}
		printf("scetween base %lld, diff %lld over %lld ms, ease %s, delay %lld\n",
			(long long)options.from, (long long)scetween.diff,
			(long long)scetween.durationMs, fx::EaseName(scetween.ease),
			(long long)scetween.delayMs);
		std::vector<long long> times = options.at;
		if(times.empty())
			times = { 0, 100, 200, 400 };
		for(long long at : times)
			printf("  t=%-6lld %lld\n", at,
				(long long)fx::ScetweenValueAt(scetween, options.from, at));
		// `diff` is a delta: a second add starts wherever the first left the
		// layer.  This is the line that catches an implementation treating `diff`
		// as an absolute value.
		const long long span = scetween.durationMs + scetween.delayMs;
		const long long once = fx::ScetweenValueAt(scetween, options.from, span);
		const long long twice = fx::ScetweenValueAt(scetween, once, span);
		printf("  chained  %lld -> %lld -> %lld\n", (long long)options.from, once, twice);
		return 0;
	}

	int CommandLytween(const std::string& path)
	{
		artemis::IetScript script;
		std::string error;
		if(!LoadScript(path, script, error))
		{
			fprintf(stderr, "fxtool: %s\n", error.c_str());
			return 1;
		}
		LytweenReport report;
		CollectLytween(script, report);
		printf("%s  [lytween] %d tags: %d usable, %d refused, "
			"%d yoyo=-1, %d finite yoyo\n", path.c_str(), report.tags, report.usable,
			report.refused, report.yoyoForever, report.yoyoFinite);
		for(const fx::LytweenTag& tag : report.parsed)
		{
			printf("  id=%-6s %-7s %lld -> %-6lld over %-6lld ms ease %-16s yoyo %d\n",
				tag.id.c_str(), tag.param.c_str(), (long long)tag.tween.from,
				(long long)tag.tween.to, (long long)tag.tween.durationMs,
				fx::EaseName(tag.tween.ease), tag.tween.yoyo);
		}
		for(const std::string& note : report.notes)
			printf("  refused %s\n", note.c_str());
		return 0;
	}

	int CommandAnime(const std::string& path, const Options& options)
	{
		if(HasSuffix(path, ".ipt"))
		{
			std::string text;
			if(!ReadWholeFile(path, text))
			{
				fprintf(stderr, "fxtool: cannot read %s\n", path.c_str());
				return 1;
			}
			artemis::Ipt ipt;
			std::string error;
			if(!artemis::ParseIpt(text, ipt, error))
			{
				fprintf(stderr, "fxtool: %s\n", error.c_str());
				return 1;
			}
			fx::AnimeFrames frames;
			frames.count = (int)ipt.frames.size();
			frames.frameMs = ipt.frameTime;
			frames.maxCycles = ipt.frameMax;
			frames.loop = ipt.frameLoop;
			printf("%s  dialect %s, mode \"%s\", canvas %dx%d, anchor %d,%d\n", path.c_str(),
				artemis::Name(ipt.dialect), ipt.mode.c_str(), ipt.width, ipt.height,
				ipt.anchorX, ipt.anchorY);
			printf("  frames %d (", frames.count);
			for(size_t i = 0; i < ipt.frames.size(); i++)
				printf("%s%s", i == 0 ? "" : ", ", ipt.frames[i].c_str());
			printf("), time %d ms, max %d, loop %d\n", frames.frameMs, frames.maxCycles,
				frames.loop);
			printf("  cycle %lld ms\n", (long long)fx::AnimeCycleMs(frames));
			std::vector<long long> times = options.at;
			if(times.empty())
				times = { 0, 100, 250, 400 };
			for(long long at : times)
			{
				const fx::FrameSample sample = fx::SampleFrames(frames, at);
				printf("  t=%-6lld frame %d, cycle %d%s\n", at, sample.frame, sample.cycle,
					sample.ended ? ", ended (holding the last frame)" : "");
			}
			return 0;
		}

		artemis::IetScript script;
		std::string error;
		if(!LoadScript(path, script, error))
		{
			fprintf(stderr, "fxtool: %s\n", error.c_str());
			return 1;
		}
		TrackReport report;
		BuildTrack(script, options.id, report);
		if(report.track.keys.empty())
		{
			fprintf(stderr, "fxtool: no [anime id=\"%s\"] in %s\n", options.id.c_str(),
				path.c_str());
			return 2;
		}
		printf("%s  [anime id=\"%s\"]: %d init + %d add + %d end\n", path.c_str(),
			options.id.c_str(), report.initTags, report.addTags, report.endTags);
		printf("  duration %lld ms (end hold %lld ms)\n",
			(long long)fx::AnimeTrackDurationMs(report.track, 0),
			(long long)report.track.endHoldMs);
		long long walked = 0;
		for(size_t i = 0; i < report.track.keys.size(); i++)
		{
			const fx::AnimeKey& key = report.track.keys[i];
			printf("  key %zu at %-5lld holds %-5lld %-22s top %d left %d alpha %d\n", i,
				walked, (long long)fx::AnimeKeyHoldMs(report.track, i),
				key.hasFile ? key.file.c_str() : "-", key.top, key.left, key.alpha);
			walked += fx::AnimeKeyHoldMs(report.track, i);
		}
		std::vector<long long> times = options.at;
		if(times.empty())
			times = { 0, 400, 900, 3600 };
		for(long long at : times)
		{
			const fx::AnimeSample sample = fx::SampleAnime(report.track, at, 0);
			printf("  t=%-6lld key %d (%lld ms into it)%s\n", at, sample.index,
				(long long)sample.msInto, sample.ended ? ", ended" : "");
		}
		return 0;
	}

	int CommandSfade(const std::string& in, const std::string& out, const Options& options)
	{
		artemis::Image image;
		std::string error;
		if(!artemis::ReadPng(in, image, error))
		{
			fprintf(stderr, "fxtool: %s\n", error.c_str());
			return 1;
		}
		const PixelStats before = Measure(image);
		int gain = (int)options.from;
		if(options.ms > 0)
			gain = fx::GainAt(options.at.empty() ? options.ms : options.at.front(),
				fx::kBrightnessIdentity, gain, options.ms);
		const auto start = std::chrono::steady_clock::now();
		fx::ApplyGain(image.rgba.data(), image.pixels(), gain);
		const auto stop = std::chrono::steady_clock::now();
		const double elapsed = std::chrono::duration<double, std::milli>(stop - start).count();
		const PixelStats after = Measure(image);
		size_t changed = 0;
		artemis::Image original;
		if(artemis::ReadPng(in, original, error) && original.rgba.size() == image.rgba.size())
		{
			for(size_t i = 0; i < image.pixels(); i++)
				if(memcmp(&original.rgba[i * 4], &image.rgba[i * 4], 4) != 0)
					changed++;
		}
		if(!artemis::WritePng(out, image.width, image.height, image.rgba.data(), error))
		{
			fprintf(stderr, "fxtool: %s\n", error.c_str());
			return 1;
		}
		printf("sfade %s -> %s  gain %d%s\n", in.c_str(), out.c_str(), gain,
			options.ms > 0 ? " (ramped from 1000)" : "");
		printf("  frame    %dx%d = %zu pixels\n", image.width, image.height, image.pixels());
		printf("  mean RGB before %.2f/%.2f/%.2f  after %.2f/%.2f/%.2f\n", before.meanR,
			before.meanG, before.meanB, after.meanR, after.meanG, after.meanB);
		printf("  mean A   before %.2f  after %.2f  (alpha must not move)\n", before.meanA,
			after.meanA);
		printf("  R range  before %d..%d  after %d..%d\n", before.minR, before.maxR,
			after.minR, after.maxR);
		printf("  colours  before %zu  after %zu\n", before.distinct, after.distinct);
		printf("  changed  %zu of %zu pixels (%.1f%%)\n", changed, image.pixels(),
			100.0 * (double)changed / (double)(image.pixels() == 0 ? 1 : image.pixels()));
		printf("  apply    %.3f ms, %.1f MiB/s\n", elapsed,
			elapsed > 0.0
				? ((double)image.pixels() * 4.0 / (1024.0 * 1024.0)) / (elapsed / 1000.0)
				: 0.0);
		return 0;
	}

	int CommandQuake(const Options& options)
	{
		fx::Quake quake;
		quake.sizePx = (int)options.from;
		quake.count = options.count;
		quake.vertical = options.vertical;
		quake.timeMs = options.ms;
		quake.seed = options.seed;
		printf("quake seed %llu, size %d, count %d, dir %s, time %lld ms\n",
			(unsigned long long)quake.seed, quake.sizePx, quake.count,
			quake.vertical ? "v" : "h", (long long)quake.timeMs);
		std::vector<long long> times = options.at;
		if(times.empty())
			times = { 0, 100 };
		for(long long at : times)
		{
			const fx::QuakeOffset offset = fx::QuakeAt(quake, at);
			printf("  t=%-6lld dx %-4d dy %-4d phase %d\n", at, offset.dx, offset.dy,
				offset.phase);
		}
		printf("  one offset per jolt:");
		const int count = quake.count > 0 ? quake.count : 1;
		for(int phase = 0; phase < count; phase++)
		{
			const long long at = ((long long)quake.timeMs * phase) / count;
			const fx::QuakeOffset offset = fx::QuakeAt(quake, at);
			printf(" %d@%lldms", quake.vertical ? offset.dy : offset.dx, at);
		}
		printf("\n");
		return 0;
	}

	// ---------------------------------------------------------------------------
	// colortone / extrans / excall / tags / guistate
	// ---------------------------------------------------------------------------
	int CommandColorTone(const Options& options, const std::vector<std::string>& positional)
	{
		fx::ColorStyle style = fx::ColorStyle::Sepia;
		if(!fx::ParseColorStyle(options.style, style))
		{
			fprintf(stderr, "fxtool: unknown colour style %s\n", options.style.c_str());
			return 2;
		}
		if(options.bench)
		{
			const int width = 1024, height = 698; // the nicolan stage
			const size_t pixels = (size_t)width * height;
			std::vector<uint8_t> stage(pixels * 4, 128);
			for(size_t at = 3; at < stage.size(); at += 4)
				stage[at] = 255;
			const int frames = options.count > 1 ? options.count : 50;
			const auto start = std::chrono::steady_clock::now();
			for(int i = 0; i < frames; i++)
				fx::ApplyColorTone(stage.data(), pixels, style);
			const auto stop = std::chrono::steady_clock::now();
			const double total = std::chrono::duration<double, std::milli>(stop - start)
				.count();
			printf("colortone %s %dx%d = %zu pixels: %.3f ms/frame over %d frames\n",
				fx::ColorStyleName(style), width, height, pixels, total / frames, frames);
			return 0;
		}
		if(positional.size() < 2)
		{
			fprintf(stderr, "fxtool: colortone needs <in.png> <out.png> [--style reset|sepia]\n");
			return 2;
		}
		artemis::Image image;
		std::string error;
		if(!artemis::ReadPng(positional[0], image, error))
		{
			fprintf(stderr, "fxtool: %s\n", error.c_str());
			return 1;
		}
		const auto start = std::chrono::steady_clock::now();
		fx::ApplyColorTone(image.rgba.data(), image.pixels(), style);
		const auto stop = std::chrono::steady_clock::now();
		const double elapsed = std::chrono::duration<double, std::milli>(stop - start).count();
		if(!artemis::WritePng(positional[1], image.width, image.height,
			image.rgba.data(), error))
		{
			fprintf(stderr, "fxtool: %s\n", error.c_str());
			return 1;
		}
		printf("colortone %s: %s -> %s, %dx%d, %d colour(s), %.3f ms\n",
			fx::ColorStyleName(style), positional[0].c_str(), positional[1].c_str(),
			image.width, image.height,
			(int)artemis::CountColors(image.rgba.data(), image.pixels()), elapsed);
		return 0;
	}

	int CommandExtrans(const Options& options, const std::vector<std::string>& positional)
	{
		if(positional.size() < 3)
		{
			fprintf(stderr, "fxtool: extrans needs <a.png> <b.png> <out.png> "
				"[--time 1000] [--at 500]\n");
			return 2;
		}
		artemis::Image a, b;
		std::string error;
		if(!artemis::ReadPng(positional[0], a, error)
			|| !artemis::ReadPng(positional[1], b, error))
		{
			fprintf(stderr, "fxtool: %s\n", error.c_str());
			return 1;
		}
		if(a.width != b.width || a.height != b.height)
		{
			fprintf(stderr, "fxtool: extrans needs two frames of the same size\n");
			return 1;
		}
		const int64_t timeMs = options.ms;
		const int64_t atMs = options.at.empty() ? timeMs / 2 : options.at.front();
		std::vector<uint8_t> out(a.rgba.size());
		fx::MixFrame(a.rgba.data(), b.rgba.data(), a.pixels(), atMs, timeMs, out.data());
		if(!artemis::WritePng(positional[2], a.width, a.height, out.data(), error))
		{
			fprintf(stderr, "fxtool: %s\n", error.c_str());
			return 1;
		}
		size_t changedA = 0, equalsA = 0, equalsB = 0;
		for(size_t i = 0; i < a.pixels(); i++)
		{
			if(memcmp(&out[i * 4], &a.rgba[i * 4], 4) == 0)
				equalsA++;
			if(memcmp(&out[i * 4], &b.rgba[i * 4], 4) == 0)
				equalsB++;
			if(memcmp(&out[i * 4], &a.rgba[i * 4], 4) != 0)
				changedA++;
		}
		printf("extrans %s + %s -> %s at t=%lld/%lld ms (weight %lld/1024)\n",
			positional[0].c_str(), positional[1].c_str(), positional[2].c_str(),
			(long long)atMs, (long long)timeMs, (long long)fx::MixWeight(atMs, timeMs));
		printf("  %dx%d, A pixels %zu, B pixels %zu, changed from A %zu, %d colour(s)\n",
			a.width, a.height, equalsA, equalsB, changedA,
			(int)artemis::CountColors(out.data(), a.pixels()));
		return 0;
	}

	int CommandExcall(const std::vector<std::string>& positional)
	{
		const Corpus corpus = FindCorpus();
		if(corpus.astDir.empty())
		{
			fprintf(stderr, "fxtool: excall needs testdata/artemis-henpri-trial/"
				"extracted/script\n");
			return 2;
		}
		const std::string start = positional.empty() ? "d09_hy01_1" : positional[0];
		auto loader = [&corpus](const std::string& file, fx::ScriptProgram& out,
			std::string& error) {
			return LoadAstProgram(corpus.astDir, file, out, error);
		};
		const fx::ExcallWalk walk = fx::WalkExcall(start, "top", loader);
		printf("excall %s:top from %s\n", start.c_str(), corpus.astDir.c_str());
		printf("  calls %d, returns %d, depth %d, blocks %d, commands %d, files %d\n",
			walk.calls, walk.returns, walk.maxDepth, walk.blocks, walk.commands,
			walk.filesLoaded);
		printf("  unresolved %d, missing labels %d, cycles %d, return commands %d\n",
			walk.unresolved, walk.missingLabels, walk.cycles, walk.returnCommands);
		printf("  call chain:");
		int shown = 0;
		for(const std::string& event : walk.events)
		{
			if(event.rfind("call:", 0) != 0)
				continue;
			if(shown++ >= 16)
			{
				printf(" ...");
				break;
			}
			printf(" %s", event.c_str());
		}
		printf("\n");
		if(!walk.unresolvedFiles.empty())
		{
			printf("  first unresolved targets:");
			for(size_t i = 0; i < walk.unresolvedFiles.size() && i < 8; i++)
				printf(" %s", walk.unresolvedFiles[i].c_str());
			if(walk.unresolvedFiles.size() > 8)
				printf(" ...");
			printf("\n");
			printf("  note: the trial volume holds 59 of the scripts; the rest are in "
				"later volumes\n");
		}
		return walk.limitHit ? 1 : 0;
	}

	int CommandTags(const std::vector<std::string>& positional)
	{
		const Corpus corpus = FindCorpus();
		const std::string directory = positional.empty() ? corpus.astDir : positional[0];
		AstCorpusStats stats;
		std::string error;
		if(!ScanAstCorpus(directory, stats, error))
		{
			fprintf(stderr, "fxtool: %s\n", error.c_str());
			return 1;
		}
		printf("ast corpus %s: %d file(s), %d parse failure(s)\n", directory.c_str(),
			stats.files, stats.parseFailures);
		printf("  colortone %d (reset %d, sepia %d), fg style=sepia %d\n",
			stats.colortone, stats.colortoneReset, stats.colortoneSepia, stats.fgSepia);
		printf("  extrans %d (no time %d, sync=0 %d), excall %d (%zu unique targets, "
			"label= %d)\n", stats.extrans, stats.extransNoTime, stats.extransSync0,
			stats.excall, stats.excallTargets.size(), stats.excallWithLabel);
		printf("  extrans times:");
		for(const auto& entry : stats.extransTimeCounts)
			printf(" %lldms x%d,", (long long)entry.first, entry.second);
		printf("\n");
		printf("  movie %d (cancel %d, bg=black %d):", stats.movie, stats.movieCancel,
			stats.movieBlackBg);
		for(const fx::MovieTag& tag : stats.movies)
			printf(" %s", tag.file.c_str());
		printf("\n");
		printf("  タイトル %d, autoplay %d (stop %d), skipstop %d\n", stats.titleTag,
			stats.autoplay, stats.autoplayStop, stats.skipstop);
		printf("  cacheclear %d, exfont %d (sized %d, colour %d), savetitle %d "
			"(%zu distinct)\n", stats.cacheclear, stats.exfont, stats.exfontSized,
			stats.exfontColour, stats.savetitle, stats.savetitleTexts.size());
		printf("  exfont sizes:");
		for(const auto& entry : stats.exfontSizes)
			printf(" %s x%d,", entry.first.c_str(), entry.second);
		printf(" colours %zu distinct\n", stats.exfontColours.size());
		printf("  delay-scheduled commands %d, inline exfont tags %d\n",
			stats.delayCommands, stats.inlineExfont);
		fx::SaveTitleBook book;
		for(const std::string& title : stats.savetitleAll)
			book.Set(title);
		printf("  save titles: %zu sets, %zu distinct, current %zu byte(s)\n", book.Sets(),
			book.UniqueTitles(), book.Current().size());
		fx::TextureCache cache;
		cache.Insert("bg", 4u << 20);
		cache.Insert("fg", 3u << 20);
		const fx::TextureCache::ClearResult freed = cache.Clear();
		printf("  cacheclear demo: freed %zu texture(s), %zu byte(s)\n", freed.textures,
			freed.bytes);
		return 0;
	}

	int CommandGuistate(const std::vector<std::string>& positional)
	{
		const Corpus corpus = FindCorpus();
		const std::string path = positional.empty() ? corpus.first : positional[0];
		GuiScriptStats stats;
		std::string error;
		if(!ScanGuiScript(path, stats, error))
		{
			fprintf(stderr, "fxtool: %s\n", error.c_str());
			return 1;
		}
		printf("guistate %s: %d active definition(s), %d commented, %d init, %d add\n",
			path.c_str(), stats.total, stats.commented, stats.init, stats.add);
		printf("  types:");
		for(const auto& entry : stats.types)
			printf(" %s x%d,", entry.first.c_str(), entry.second);
		printf("\n  time spellings: time=%d, msec=%d\n", stats.timeSpelling,
			stats.msecSpelling);
		fx::GuiStateMap map = fx::GuiStateMap::Default();
		int applied = 0;
		for(const fx::GuiDefinition& definition : stats.definitions)
			if(map.Apply(definition))
				applied++;
		printf("  applied %d of %d definitions to the table\n", applied, stats.total);
		printf("  %-18s %-20s %-34s %s\n", "slot", "layer role", "property init->target",
			"source");
		for(const fx::GuiSlot& slot : map.Slots())
		{
			std::string properties;
			for(const fx::GuiProperty& property : slot.properties)
			{
				if(!properties.empty())
					properties += ", ";
				properties += property.param + " " + std::to_string(property.init) + "->"
					+ std::to_string(property.target);
			}
			printf("  %-18s %-20s %-34s %s\n", slot.state.c_str(), slot.layer.c_str(),
				properties.c_str(), slot.observed ? "real" : "inferred");
		}
		return 0;
	}

	// ---------------------------------------------------------------------------
	// selftest
	// ---------------------------------------------------------------------------
	void TestTween();
	void TestTypewriter();
	void TestQuake();
	void TestBrightness(const Corpus& corpus);
	void TestLytween(const Corpus& corpus);
	void TestAnimation(const Corpus& corpus);
	void TestPages(const Corpus& corpus);
	void TestColorTone(const AstCorpusStats* ast);
	void TestExtrans(const AstCorpusStats* ast);
	void TestExcall(const Corpus& corpus, const AstCorpusStats* ast);
	void TestTags(const Corpus& corpus, const AstCorpusStats* ast);
	void TestGuiState(const Corpus& corpus);

	int RunSelfTest()
	{
		const Corpus corpus = FindCorpus();
		printf("fxtool selftest\n");
		printf("corpus: %s\n",
			corpus.ok() ? corpus.root.c_str() : "(absent: the real-data checks skip)");
		printf("\n");

		TestTween();
		TestTypewriter();
		TestQuake();
		TestBrightness(corpus);
		TestLytween(corpus);
		TestAnimation(corpus);
		TestPages(corpus);

		AstCorpusStats ast;
		std::string astError;
		const bool haveAst = !corpus.astDir.empty()
			&& ScanAstCorpus(corpus.astDir, ast, astError);
		if(!corpus.astDir.empty() && !haveAst)
			printf("  (the .ast corpus could not be read: %s)\n", astError.c_str());
		TestColorTone(haveAst ? &ast : nullptr);
		TestExtrans(haveAst ? &ast : nullptr);
		TestExcall(corpus, haveAst ? &ast : nullptr);
		TestTags(corpus, haveAst ? &ast : nullptr);
		TestGuiState(corpus);

		printf("\n%d passed, %d failed, %d skipped\n", g_test.passed, g_test.failed,
			g_test.skipped);
		for(const std::string& failure : g_test.failures)
			printf("  FAILED %s\n", failure.c_str());
		return g_test.failed == 0 ? 0 : 1;
	}

	void TestTween()
	{
		printf("-- tween\n");
		// Mutation spot: make ValueAt return `to` always, or drop the +0.5
		// rounding, and these exact values move.
		fx::Tween ramp;
		ramp.from = 0;
		ramp.to = 255;
		ramp.durationMs = 1000;
		ramp.ease = fx::Ease::Linear;
		CheckEq("tween-start", fx::ValueAt(ramp, 0), 0);
		CheckEq("tween-half-rounds-up", fx::ValueAt(ramp, 500), 128); // 127.5 -> 128
		CheckEq("tween-end", fx::ValueAt(ramp, 1000), 255);
		CheckEq("tween-past-end", fx::ValueAt(ramp, 5000), 255);
		CheckEq("tween-before-start", fx::ValueAt(ramp, -50), 0);

		fx::Tween negative = ramp;
		negative.to = -255;
		CheckEq("tween-negative-half", fx::ValueAt(negative, 500), -128); // symmetric

		// The five easings the corpus actually uses, sampled on a 0..1000 ramp.
		// The numbers are printed as a table because a renderer has to match them,
		// not merely pass a test.
		struct Sample
		{
			const char* name;
			long long expect[5];
		};
		const Sample samples[] = {
			{ "none", { 0, 250, 500, 750, 1000 } },
			{ "easeout_sine", { 0, 383, 707, 924, 1000 } },
			{ "easeout_quad", { 0, 438, 750, 938, 1000 } },
			{ "easeout_quart", { 0, 684, 938, 996, 1000 } },
			{ "easeinout_quart", { 0, 31, 500, 969, 1000 } },
		};
		const long long times[5] = { 0, 250, 500, 750, 1000 };
		printf("   ease              t=0    t=250  t=500  t=750  t=1000\n");
		for(const Sample& sample : samples)
		{
			fx::Ease ease = fx::Ease::Linear;
			const bool known = fx::ParseEase(sample.name, ease);
			Check(sample.name, known, known ? "parsed" : "NOT PARSED");
			if(!known)
				continue;
			fx::Tween shaped = ramp;
			shaped.to = 1000;
			shaped.ease = ease;
			char line[192];
			char cell[5][16];
			bool ok = true;
			for(int i = 0; i < 5; i++)
			{
				const long long got = fx::ValueAt(shaped, times[i]);
				snprintf(cell[i], sizeof(cell[i]), "%lld", got);
				if(got != sample.expect[i])
					ok = false;
			}
			snprintf(line, sizeof(line), "%-16s %6s %6s %6s %6s %6s", sample.name, cell[0],
				cell[1], cell[2], cell[3], cell[4]);
			const std::string label = std::string("ease-") + sample.name;
			Check(label.c_str(), ok, line);
		}

		fx::Ease unknown = fx::Ease::Linear;
		Check("ease-unknown-reported", !fx::ParseEase("easeout_bogus", unknown),
			"an unknown curve is refused, not silently linear");

		// delay: the real scetween passes delay="$g.msgspeed" (30)
		fx::Tween delayed = ramp;
		delayed.to = 100;
		delayed.durationMs = 100;
		delayed.delayMs = 30;
		CheckEq("tween-delay-holds", fx::ValueAt(delayed, 0), 0);
		CheckEq("tween-delay-last-ms", fx::ValueAt(delayed, 29), 0);
		CheckEq("tween-delay-starts", fx::ValueAt(delayed, 30), 0);
		CheckEq("tween-delay-mid", fx::ValueAt(delayed, 80), 50);
		CheckEq("tween-delay-done", fx::ValueAt(delayed, 130), 100);

		// yoyo: one pass forward, one back.  -1 never stops.
		fx::Tween pingpong = ramp;
		pingpong.to = 100;
		pingpong.durationMs = 1000;
		pingpong.yoyo = -1;
		CheckEq("yoyo-forever-1st", fx::ValueAt(pingpong, 1000), 100);
		CheckEq("yoyo-forever-back-mid", fx::ValueAt(pingpong, 1500), 50);
		CheckEq("yoyo-forever-back-end", fx::ValueAt(pingpong, 2000), 0);
		CheckEq("yoyo-forever-2nd-cycle", fx::ValueAt(pingpong, 3000), 100);
		Check("yoyo-forever-never-stops", pingpong.totalMs() < 0, "totalMs() < 0");

		// yoyo counts *reversals*, so the pass count is 1 + yoyo.  yoyo=1 is two
		// passes and lands back on `from`; yoyo=30 and yoyo=10 are odd pass counts
		// and land on `to`.  See the header of the deliverable for why this is the
		// reading: `yoyo="-1"` in the real data is a permanent blink, and the small
		// numbers (10, 30) are finite blinks that must return to rest.
		fx::Tween finite = pingpong;
		finite.yoyo = 1;
		CheckEq("yoyo-2-passes-end", fx::ValueAt(finite, 2000), 0);
		CheckEq("yoyo-2-passes-stop", finite.totalMs(), 2000);
		CheckEq("yoyo-2-passes-frozen", fx::ValueAt(finite, 9000), 0);
		finite.yoyo = 30;
		CheckEq("yoyo-30-passes-end", fx::ValueAt(finite, 31 * 1000), 100);
		CheckEq("yoyo-30-stops-at", finite.totalMs(), 31000);

		// clamp: a curve can never take the value outside [from,to]
		bool inside = true;
		fx::Tween wide = ramp;
		wide.from = 100;
		wide.to = -100;
		wide.durationMs = 400;
		wide.ease = fx::Ease::InOutQuart;
		for(int t = -20; t <= 500; t++)
		{
			const long long v = fx::ValueAt(wide, t);
			if(v > 100 || v < -100)
				inside = false;
		}
		Check("tween-clamped", inside, "100 -> -100 never leaves the span");
		CheckEq("tween-span-descends", fx::ValueAt(wide, 200), 0);

		// scetween is a *delta*: two adds accumulate.  Mutation spot: return
		// `base + diff` unconditionally, or treat `diff` as absolute, and this
		// chain comes out 745/745 instead of 1000/745/490.
		fx::Scetween scetween;
		scetween.diff = -255;
		scetween.durationMs = 400;
		CheckEq("scetween-halfway", fx::ScetweenValueAt(scetween, 1000, 200), 872);
		CheckEq("scetween-once", fx::ScetweenValueAt(scetween, 1000, 400), 745);
		CheckEq("scetween-twice", fx::ScetweenValueAt(scetween, 745, 400), 490);
		// the tween itself does not know the property's range: an alpha ramp can
		// ask for -55, and it is the layer that clamps.  Both halves are pinned so
		// neither the ramp nor the clamp can drift silently.
		CheckEq("scetween-overshoots", fx::ScetweenValueAt(scetween, 200, 400), -55);
		CheckEq("scetween-clamped-at-zero",
			fx::ClampProperty(fx::ScetweenValueAt(scetween, 200, 400), 0, 255), 0);
		fx::Scetween withDelay = scetween;
		withDelay.delayMs = 30;
		CheckEq("scetween-delay", fx::ScetweenValueAt(withDelay, 1000, 30), 1000);
		CheckEq("scetween-delay-mid", fx::ScetweenValueAt(withDelay, 1000, 230), 872);
	}

	void TestTypewriter()
	{
		printf("-- typewriter\n");
		// Mutation spot: cut on bytes instead of glyphs ("substr(0, t/30)") and
		// typewriter-boundary goes red, because 日本語 would be cut inside a
		// character at t=10 and t=20.
		const fx::Text japanese = fx::Segment("日本語");
		CheckEq("segment-glyphs", (long long)japanese.timedGlyphs, 3);
		if(japanese.stops.size() == 3)
		{
			CheckEq("segment-stop-1", (long long)japanese.stops[0], 3);
			CheckEq("segment-stop-3", (long long)japanese.stops[2], 9);
		}
		CheckEq("reveal-t0", (long long)fx::RevealAt(japanese, 0, 30).bytes, 3);
		CheckEq("reveal-t30", (long long)fx::RevealAt(japanese, 30, 30).bytes, 6);
		CheckEq("reveal-t60", (long long)fx::RevealAt(japanese, 60, 30).bytes, 9);
		Check("reveal-complete", fx::RevealAt(japanese, 60, 30).complete, "complete at 60 ms");
		CheckEq("reveal-duration", (long long)fx::RevealDurationMs(japanese, 30), 60);

		// every reachable cut is a glyph boundary: no 1, 2, 4, 5, 7 or 8
		bool onBoundary = true;
		std::string worst;
		for(int t = 0; t <= 120; t++)
		{
			const fx::Reveal reveal = fx::RevealAt(japanese, t, 30);
			if(reveal.bytes % 3 != 0)
			{
				onBoundary = false;
				worst = "t=" + std::to_string(t) + " cut at byte " +
					std::to_string(reveal.bytes);
			}
			if(reveal.shown.find("\xEF\xBF\xBD") != std::string::npos)
			{
				onBoundary = false;
				worst = "t=" + std::to_string(t) + " produced U+FFFD";
			}
		}
		Check("typewriter-boundary", onBoundary,
			onBoundary ? "no cut ever lands inside 漢 (byte counts are 0/3/6/9)" : worst);

		// the renderer gets a prefix, never a re-encoding
		const fx::Reveal cut = fx::RevealAt(japanese, 30, 30);
		Check("typewriter-prefix", japanese.utf8.compare(0, cut.bytes, cut.shown) == 0,
			"shown is a prefix of the page text");

		// a line break costs no time but is already in effect
		const fx::Text lines = fx::Segment("あ\nい");
		CheckEq("break-not-timed", (long long)lines.timedGlyphs, 2);
		CheckStr("break-at-t0", fx::RevealAt(lines, 0, 30).shown, "あ\n");
		CheckStr("break-at-t30", fx::RevealAt(lines, 30, 30).shown, "あ\nい");

		// a combining dakuten belongs to the kana in front of it
		const fx::Text combined = fx::Segment("か\xE3\x82\x99");
		CheckEq("combining-one-glyph", (long long)combined.timedGlyphs, 1);
		CheckEq("combining-duration", (long long)fx::RevealDurationMs(combined, 30), 0);
		CheckEq("combining-bytes", (long long)combined.glyphs[0].byteLength, 6);
		CheckEq("combining-codepoints", (long long)combined.glyphs[0].codePoints, 2);

		// the "instant" text speed
		const fx::Reveal instant = fx::RevealAt(japanese, 0, 0);
		Check("typewriter-instant", instant.complete && instant.glyphs == 3, "msgspeed=0");
		// negative time (a page that just appeared, called with t<0)
		CheckEq("reveal-negative-t", (long long)fx::RevealAt(japanese, -100, 30).bytes, 3);

		// broken bytes must not lose the string or crash
		std::string broken = "a";
		broken += (char)0xFF;
		broken += "b";
		const fx::Text brokenText = fx::Segment(broken);
		CheckEq("broken-bytes", (long long)brokenText.badBytes, 1);
		CheckEq("broken-glyphs", (long long)brokenText.timedGlyphs, 3);
		CheckEq("broken-stops-cover-input", (long long)brokenText.stops.back(), 3);

		const fx::Text empty = fx::Segment("");
		Check("empty-page", empty.glyphs.empty() && empty.timedGlyphs == 0, "no glyphs");
		Check("empty-page-complete", fx::RevealAt(empty, 0, 30).complete, "complete at once");
	}

	void TestQuake()
	{
		printf("-- quake\n");
		fx::Quake quake;
		quake.sizePx = 30;
		quake.count = 2;
		quake.timeMs = 50;
		quake.seed = 7;

		const fx::QuakeOffset first = fx::QuakeAt(quake, 0);
		CheckEq("quake-first-phase", first.phase, 0);
		CheckEq("quake-first-amplitude", first.dx < 0 ? -first.dx : first.dx, 30);
		CheckEq("quake-horizontal-only", first.dy, 0);
		Check("quake-first-jolt-active", first.active(), "phase 0 is a jolt");

		const fx::QuakeOffset last = fx::QuakeAt(quake, 25);
		CheckEq("quake-second-phase", last.phase, 1);
		CheckEq("quake-decays", last.dx < 0 ? -last.dx : last.dx, 15); // 30*(2-1)/2

		const fx::QuakeOffset after = fx::QuakeAt(quake, 50);
		CheckEq("quake-rest-at-end", after.dx, 0);
		CheckEq("quake-rest-phase", after.phase, -1);
		CheckEq("quake-rest-late", fx::QuakeAt(quake, 9999).dx, 0);
		CheckEq("quake-before-start", fx::QuakeAt(quake, -1).phase, -1);

		// the same seed must replay exactly, in any order, any number of calls
		bool reproducible = true;
		for(int t = 0; t < 50; t++)
		{
			const fx::QuakeOffset a = fx::QuakeAt(quake, t);
			const fx::QuakeOffset b = fx::QuakeAt(quake, t);
			if(a.dx != b.dx || a.dy != b.dy || a.phase != b.phase)
				reproducible = false;
		}
		Check("quake-same-seed", reproducible, "50 samples, asked twice, identical");
		Check("quake-hash-is-pure",
			fx::QuakeHash(7, 3) == fx::QuakeHash(7, 3) &&
				fx::QuakeHash(7, 3) != fx::QuakeHash(7, 4),
			"hash(7,3) stable, and different from hash(7,4)");

		// a different seed must actually change the picture, or replaying the same
		// scene would shake identically every time
		fx::Quake other = quake;
		other.seed = 8;
		bool differs = false;
		for(int t = 0; t < 50; t++)
			if(fx::QuakeAt(quake, t).dx != fx::QuakeAt(other, t).dx)
				differs = true;
		Check("quake-seed-changes", differs, "seed 7 and seed 8 disagree");

		// amplitude is bounded and always decays to 0
		bool bounded = true;
		quake.sizePx = 50;
		quake.count = 3;
		quake.timeMs = 100;
		for(int t = 0; t < 100; t++)
		{
			const fx::QuakeOffset offset = fx::QuakeAt(quake, t);
			if(offset.dx > 50 || offset.dx < -50)
				bounded = false;
		}
		Check("quake-bounded", bounded, "|offset| <= size for every ms");

		quake.vertical = true;
		bool vertical = true;
		for(int t = 0; t < 100; t++)
			if(fx::QuakeAt(quake, t).dx != 0)
				vertical = false;
		Check("quake-vertical", vertical, "dir=v only moves dy");

		// degenerate arguments must not divide by zero
		fx::Quake silly;
		silly.sizePx = 10;
		silly.count = 0;
		silly.timeMs = 0;
		Check("quake-degenerate", fx::QuakeAt(silly, 10).phase == -1, "no jolt, no crash");
	}

	void TestBrightness(const Corpus& corpus)
	{
		printf("-- brightness (sfade)\n");
		// Mutation spot: forget the +500 and 255/2 becomes 127 instead of 128;
		// scale alpha too and brightness-alpha-untouched goes red.
		bool identity = true;
		for(int v = 0; v < 256; v++)
			if(fx::ScaleChannel((uint8_t)v, 1000) != (uint8_t)v)
				identity = false;
		Check("brightness-identity", identity, "gain 1000 returns all 256 values unchanged");
		CheckEq("brightness-500-max", fx::ScaleChannel(255, 500), 128); // 127.5 rounds up
		CheckEq("brightness-500-zero", fx::ScaleChannel(0, 500), 0);
		CheckEq("brightness-500-100", fx::ScaleChannel(100, 500), 50); // 50.5 rounds down
		CheckEq("brightness-zero", fx::ScaleChannel(200, 0), 0);
		CheckEq("brightness-2000-clips", fx::ScaleChannel(200, 2000), 255);
		CheckEq("brightness-clamp-negative", fx::ScaleChannel(200, -50), 0);
		CheckEq("brightness-clamp-high", fx::ClampGain(99999), fx::kBrightnessMaxGain);

		// the fade over time: 1000 -> 500 in 2000 ms is the tanabata sfade
		CheckEq("gainat-0", fx::GainAt(0, 1000, 500, 2000), 1000);
		CheckEq("gainat-half", fx::GainAt(1000, 1000, 500, 2000), 750);
		CheckEq("gainat-end", fx::GainAt(2000, 1000, 500, 2000), 500);
		CheckEq("gainat-past", fx::GainAt(9999, 1000, 500, 2000), 500);
		CheckEq("gainat-instant", fx::GainAt(0, 1000, 200, 0), 200);

		// a synthetic frame: alpha untouched, RGB through the formula
		std::vector<uint8_t> frame(16 * 4);
		for(int i = 0; i < 16; i++)
		{
			frame[(size_t)i * 4 + 0] = 255;
			frame[(size_t)i * 4 + 1] = 100;
			frame[(size_t)i * 4 + 2] = 0;
			frame[(size_t)i * 4 + 3] = (uint8_t)(100 + i);
		}
		fx::ApplyGain(frame.data(), 16, 500);
		bool alphaKept = true;
		bool rgbScaled = true;
		for(int i = 0; i < 16; i++)
		{
			if(frame[(size_t)i * 4 + 3] != (uint8_t)(100 + i))
				alphaKept = false;
			if(frame[(size_t)i * 4 + 0] != 128 || frame[(size_t)i * 4 + 1] != 50 ||
				frame[(size_t)i * 4 + 2] != 0)
				rgbScaled = false;
		}
		Check("brightness-alpha-untouched", alphaKept, "16 pixels, alpha 100..115 unchanged");
		Check("brightness-rgb-formula", rgbScaled, "255/100/0 -> 128/50/0 at gain 500");

		// gain 1000 must not even touch the buffer (it returns early)
		std::vector<uint8_t> same(frame);
		fx::ApplyGain(same.data(), 16, 1000);
		Check("brightness-identity-buffer", same == frame, "gain 1000 is a no-op");

		if(!corpus.ok() || corpus.background.empty())
		{
			Skip("sfade-real-frame", "no corpus");
			return;
		}

		artemis::Image image;
		std::string error;
		if(!artemis::ReadPng(corpus.background, image, error))
		{
			Skip("sfade-real-frame", error);
			return;
		}
		CheckEq("sfade-stage-width", image.width, 1024);
		CheckEq("sfade-stage-height", image.height, 698);
		const PixelStats before = Measure(image);
		const artemis::Image original = image;
		fx::ApplyGain(image.rgba.data(), image.pixels(), 500);
		const PixelStats after = Measure(image);

		char detail[256];
		snprintf(detail, sizeof(detail), "mean R %.1f -> %.1f, mean A %.1f -> %.1f",
			before.meanR, after.meanR, before.meanA, after.meanA);
		Check("sfade-halves-brightness",
			after.meanR > before.meanR / 2.0 - 1.0 &&
				after.meanR < before.meanR / 2.0 + 1.0,
			detail);
		Check("sfade-alpha-stable", after.meanA == before.meanA, "mean alpha identical");

		// every pixel must equal the formula, not just the average
		bool exact = true;
		for(size_t i = 0; i < image.pixels(); i++)
		{
			const uint8_t* src = &original.rgba[i * 4];
			const uint8_t* dst = &image.rgba[i * 4];
			for(int c = 0; c < 3; c++)
				if(dst[c] != fx::ScaleChannel(src[c], 500))
					exact = false;
			if(dst[3] != src[3])
				exact = false;
		}
		Check("sfade-pixel-exact", exact, "every pixel matches ScaleChannel(v,500)");

		// and the frame really did change (a no-op would pass every check above)
		size_t changed = 0;
		for(size_t i = 0; i < image.pixels(); i++)
			if(memcmp(&original.rgba[i * 4], &image.rgba[i * 4], 4) != 0)
				changed++;
		Check("sfade-actually-changed", changed > image.pixels() / 2,
			std::to_string(changed) + " of " + std::to_string(image.pixels()) + " pixels");

		// performance: the frame the renderer actually composites at
		std::vector<uint8_t> bench(image.rgba.size(), 96);
		const int iterations = 50;
		const auto start = std::chrono::steady_clock::now();
		for(int i = 0; i < iterations; i++)
			fx::ApplyGain(bench.data(), image.pixels(), 500);
		const auto stop = std::chrono::steady_clock::now();
		const double total = std::chrono::duration<double, std::milli>(stop - start).count();
		const double perFrame = total / (double)iterations;
		char perf[256];
		snprintf(perf, sizeof(perf), "%.3f ms/frame for %zu pixels (%.0f MiB/s)",
			perFrame, image.pixels(),
			((double)image.pixels() * 4.0 / (1024.0 * 1024.0)) / (perFrame / 1000.0));
		Check("sfade-perf-60fps", perFrame < 16.6, perf);
	}

	void TestLytween(const Corpus& corpus)
	{
		printf("-- lytween (real tags, including the broken ones)\n");
		// Mutation spot: make ParseLytween accept a tag without `to` (by defaulting
		// it to `from`, say) and lytween-refuses-broken goes red - the engine would
		// then "animate" 40 -> 40 and draw nothing, silently.
		std::vector<fx::AttrNameValue> ok = {
			{ "id", "81.0" }, { "param", "top" }, { "ease", "easeinout_quart" },
			{ "from", "40" }, { "to", "35" }, { "time", "1500" }, { "yoyo", "-1" },
		};
		fx::LytweenTag parsed;
		Check("lytween-parses-real-shape", fx::ParseLytween(ok, parsed),
			"id=81.0 param=top from=40 to=35 time=1500 yoyo=-1");
		CheckEq("lytween-from", (long long)parsed.tween.from, 40);
		CheckEq("lytween-to", (long long)parsed.tween.to, 35);
		CheckEq("lytween-time", (long long)parsed.tween.durationMs, 1500);
		CheckEq("lytween-yoyo", parsed.tween.yoyo, -1);
		CheckEq("lytween-yoyo-total", parsed.tween.totalMs(), -1); // never stops
		CheckEq("lytween-halfway", (long long)fx::ValueAt(parsed.tween, 750), 37);

		// the real broken tags: `top=` where `to=` belongs
		std::vector<fx::AttrNameValue> broken = {
			{ "id", "1" }, { "param", "yscale" }, { "from", "100" },
			{ "top", "100" }, { "time", "20000" }, { "sync", "0" },
		};
		fx::LytweenTag refused;
		Check("lytween-refuses-broken", !fx::ParseLytween(broken, refused),
			refused.note.empty() ? "(no note)" : refused.note);

		std::vector<fx::AttrNameValue> variable = {
			{ "id", "1" }, { "param", "alpha" }, { "from", "0" }, { "to", "255" },
			{ "time", "$g.msgspeed" },
		};
		fx::LytweenTag unresolved;
		Check("lytween-refuses-variable", !fx::ParseLytween(variable, unresolved),
			unresolved.note.empty() ? "(no note)" : unresolved.note);
		int64_t number = 0;
		Check("parse-int-value", !fx::ParseIntValue("$g.msgspeed", number) &&
			fx::ParseIntValue("-255", number) && number == -255,
			"a variable is not 0");

		if(!corpus.ok())
		{
			Skip("lytween-real-scene", "no corpus");
			return;
		}
		artemis::IetScript scene;
		std::string error;
		if(!LoadScript(corpus.scene, scene, error))
		{
			Skip("lytween-real-scene", error);
			return;
		}
		LytweenReport report;
		CollectLytween(scene, report);
		CheckEq("lytween-scene-tags", report.tags, 13);
		// three of the thirteen are broken: id="1" at lines 838, 839 and 841 all
		// write `top=` where `to=` belongs, so nothing says where the value goes
		CheckEq("lytween-scene-refused", report.refused, 3);
		CheckEq("lytween-scene-usable", report.usable, 10);
		CheckEq("lytween-scene-yoyo-forever", report.yoyoForever, 3);
		CheckEq("lytween-scene-yoyo-finite", report.yoyoFinite, 4);

		if(!corpus.first.empty())
		{
			artemis::IetScript first;
			if(LoadScript(corpus.first, first, error))
			{
				LytweenReport gui;
				CollectLytween(first, gui);
				// two live tags; a third is commented out and the file also has two
				// [lytweendel], which is why a naive text search counts five
				CheckEq("lytween-first-tags", gui.tags, 2);
				CheckEq("lytween-first-yoyo-forever", gui.yoyoForever, 2);
				CheckEq("lytween-first-refused", gui.refused, 0);
			}
			else
				Skip("lytween-first", error);
		}
		else
			Skip("lytween-first", "no first.iet");
	}

	void TestAnimation(const Corpus& corpus)
	{
		printf("-- animation\n");
		// --- the IPT descriptor: 3 frames, 100 ms each, loop=-1, max=3 ---
		if(corpus.ok() && !corpus.animeIpt.empty())
		{
			std::string text;
			artemis::Ipt ipt;
			std::string error;
			if(ReadWholeFile(corpus.animeIpt, text) && artemis::ParseIpt(text, ipt, error))
			{
				fx::AnimeFrames frames;
				frames.count = (int)ipt.frames.size();
				frames.frameMs = ipt.frameTime;
				frames.maxCycles = ipt.frameMax;
				frames.loop = ipt.frameLoop;
				CheckEq("ipt-dialect", (long long)ipt.dialect,
					(long long)artemis::IptDialect::Anime);
				CheckEq("ipt-frames", frames.count, 3);
				CheckEq("ipt-time", frames.frameMs, 100);
				CheckEq("ipt-max", frames.maxCycles, 3);
				CheckEq("ipt-loop", frames.loop, -1);
				CheckEq("ipt-canvas-w", ipt.width, 1280);
				CheckEq("ipt-canvas-h", ipt.height, 720);
				CheckEq("ipt-anchor-x", ipt.anchorX, 640);
				Check("ipt-frame-names", ipt.frames.size() == 3 && ipt.frames[0] == "line11" &&
					ipt.frames[1] == "line12" && ipt.frames[2] == "line13", "line11/12/13");
				CheckEq("ipt-cycle", (long long)fx::AnimeCycleMs(frames), 300);

				// Mutation spot: forget the % and the frame index grows without
				// bound; forget the loop budget and it never stops.
				CheckEq("ipt-frame-t0", fx::SampleFrames(frames, 0).frame, 0);
				CheckEq("ipt-frame-t100", fx::SampleFrames(frames, 100).frame, 1);
				CheckEq("ipt-frame-t250", fx::SampleFrames(frames, 250).frame, 2);
				CheckEq("ipt-frame-t400", fx::SampleFrames(frames, 400).frame, 1);
				CheckEq("ipt-frame-t899", fx::SampleFrames(frames, 899).frame, 2);
				const fx::FrameSample done = fx::SampleFrames(frames, 900);
				Check("ipt-stops-after-max", done.ended && done.frame == 2,
					"900 ms = 3 cycles x 300 ms, holds frame 2");
				CheckEq("ipt-cycle-count", fx::SampleFrames(frames, 750).cycle, 2);
			}
			else
				Skip("ipt-anime", error);
		}
		else
			Skip("ipt-anime", "no corpus");

		// --- the script tag: [anime id="82"] in first.iet, a glyph sparkle ---
		if(corpus.ok() && !corpus.first.empty())
		{
			artemis::IetScript script;
			std::string error;
			if(LoadScript(corpus.first, script, error))
			{
				TrackReport glyph;
				BuildTrack(script, "82", glyph);
				CheckEq("anime-82-keys", (long long)glyph.track.keys.size(), 8);
				CheckEq("anime-82-init", glyph.initTags, 1);
				CheckEq("anime-82-add", glyph.addTags, 7);
				CheckEq("anime-82-end", glyph.endTags, 1);
				CheckEq("anime-82-endhold", (long long)glyph.track.endHoldMs, 800);
				CheckEq("anime-82-first-top", glyph.track.keys[0].top, 30);
				CheckEq("anime-82-first-alpha", glyph.track.keys[0].alpha, 250);
				CheckEq("anime-82-key1-arrive", (long long)glyph.track.keys[1].arriveMs, 100);
				// the seven adds arrive at 100,200,...,700 and the end holds 800:
				// a blink that slows down frame by frame
				bool accelerating = true;
				for(size_t i = 1; i < glyph.track.keys.size(); i++)
					if(glyph.track.keys[i].arriveMs != (long long)i * 100)
						accelerating = false;
				Check("anime-82-holds", accelerating, "holds are 100,200,...,800 ms");
				// 400+500+600+300+700+200+100 = 2800 of key holds, + 800 end hold
				CheckEq("anime-82-duration",
					(long long)fx::AnimeTrackDurationMs(glyph.track, 0), 3600);

				// step, not tween: at 399 ms the first key is still up, at 400 the
				// second one has already snapped in with its own alpha - there is no
				// 250 -> 50 ramp in between (that would be [lytween])
				CheckEq("anime-step-before", fx::SampleAnime(glyph.track, 99).index, 0);
				CheckEq("anime-step-at", fx::SampleAnime(glyph.track, 100).index, 1);
				CheckEq("anime-step-alpha", glyph.track.keys[1].alpha, 200);
				CheckEq("anime-step-top", glyph.track.keys[1].top, 30);
				CheckEq("anime-step-left", glyph.track.keys[1].left, 3);
				CheckEq("anime-last-key", fx::SampleAnime(glyph.track, 3599).index, 7);
				Check("anime-ends", fx::SampleAnime(glyph.track, 3600).ended, "ended at 3600");

				// the background slide track in the scenario
				artemis::IetScript scene;
				TrackReport slide;
				if(!corpus.scene.empty() && LoadScript(corpus.scene, scene, error))
				{
					BuildTrack(scene, "1", slide);
					CheckEq("anime-1-keys", (long long)slide.track.keys.size(), 2);
					CheckEq("anime-1-add", slide.addTags, 1);
					CheckStr("anime-1-file0", slide.track.keys[0].file,
						std::string("bgimage/run"));
					CheckStr("anime-1-file1", slide.track.keys[1].file,
						std::string("bgimage/run2"));
					CheckEq("anime-1-duration",
						(long long)fx::AnimeTrackDurationMs(slide.track, 0), 1500);
				}
				else
					Skip("anime-1-slide", "no scene.iet");
			}
			else
				Skip("anime-82", error);
		}
		else
			Skip("anime-82", "no corpus");

		// degenerate: an empty track must report ended, not crash
		fx::AnimeTrack none;
		Check("anime-empty-track", fx::SampleAnime(none, 0).ended, "ended");
		CheckEq("anime-empty-duration", (long long)fx::AnimeTrackDurationMs(none, -1), 0);
	}

	void TestPages(const Corpus& corpus)
	{
		printf("-- pages\n");
		// synthetic: the exact rules render/vm.cpp runs
		std::vector<fx::FeedItem> feed;
		feed.push_back(fx::FeedItem::Text_("one", 1));
		feed.push_back(fx::FeedItem::Command_("rp", 1));
		feed.push_back(fx::FeedItem::Command_("rp", 2)); // an empty page must not wait
		feed.push_back(fx::FeedItem::Text_("two@three", 3));
		feed.push_back(fx::FeedItem::Command_("ruby", 3));
		feed.push_back(fx::FeedItem::Text_("base", 3));
		feed.push_back(fx::FeedItem::Command_("/ruby", 3));
		feed.push_back(fx::FeedItem::Text_("tail", 3));
		feed.push_back(fx::FeedItem::Command_("rp", 3));
		fx::PageFeedStats stats;
		const std::vector<fx::Page> pages = fx::SplitPages(feed, &stats);
		CheckEq("pages-count", (long long)pages.size(), 2);
		CheckEq("pages-empty-dropped", stats.emptyDropped, 1);
		CheckEq("pages-inline-dropped", stats.inlineDropped, 2);
		if(pages.size() == 2)
		{
			CheckStr("pages-first", pages[0].text, "one");
			// '@' became a page line break; the ruby tags left no text of their own,
			// and the three Text items on line 3 stayed on one line
			CheckStr("pages-at-is-newline", pages[1].text, "two\nthreebasetail");
		}

		// the byte trap: in cp932 the second byte of a two-byte character is often
		// 0x40 ('@'), so a byte scan counts breaks that are not there
		const std::string sjis = "\x89\xb4"; // 俺 is 89 B4 in cp932
		const std::string decoded =
			artemis::SjisToUtf8((const uint8_t*)sjis.data(), sjis.size());
		Check("pages-sjis-ascii-safety", decoded.size() == 3,
			"俺: 2 Shift-JIS bytes, 3 UTF-8 bytes");

		if(!corpus.ok())
		{
			Skip("pages-real-scene", "no corpus");
			return;
		}

		PagedScript paged;
		std::string error;
		if(!PageScript(corpus.scene, paged, error))
		{
			Skip("pages-real-scene", error);
			return;
		}
		// every [rp] closes at most one page, and a page with no text closes none
		// 409 [rp] plus 11 [rp2] in the real scene; every one of them closes a
		// page, and none of them is empty
		CheckEq("scene-pages", paged.stats.pages + paged.stats.emptyDropped, 420);
		Check("scene-page-count", paged.stats.pages > 380,
			std::to_string(paged.stats.pages) + " pages");
		CheckEq("scene-inline-dropped", paged.stats.inlineDropped, 33);

		// the real first page, byte for byte
		CheckStr("scene-page-0", paged.pages.empty() ? std::string() : paged.pages[0].text,
			"俺がまだ、ただの少年だったころ、こんな夢を見たことがある。");
		// '@' is a line break inside the page
		Check("scene-page-1-lines",
			paged.pages.size() > 1 &&
				paged.pages[1].text ==
					"そう、たぶん、七夕だったんだろう。\n彼女と、こんな話をした。",
			paged.pages.size() > 1 ? OneLine(paged.pages[1].text) : std::string("missing"));

		// the furigana in the real script must not enter the body text
		bool foundRuby = false;
		bool leakedFurigana = false;
		for(const fx::Page& page : paged.pages)
		{
			if(page.text.find("織姫") != std::string::npos)
				foundRuby = true;
			if(page.text.find("おりひめ") != std::string::npos ||
				page.text.find("ひこぼし") != std::string::npos)
				leakedFurigana = true;
		}
		Check("scene-ruby-base-kept", foundRuby, "織姫 is in the body text");
		Check("scene-ruby-dropped", !leakedFurigana, "おりひめ/ひこぼし stayed out of it");

		// why the '@' scan must run on decoded text: the raw bytes have more '@'
		Check("scene-at-raw-vs-decoded", paged.rawAtSigns > paged.atSigns,
			std::to_string(paged.atSigns) + " real breaks, " +
				std::to_string(paged.rawAtSigns) + " '@' bytes in the Shift-JIS file");

		// a real page through the typewriter at the title's own 30 ms/glyph
		if(!paged.texts.empty())
		{
			const fx::Text& first = paged.texts[0];
			CheckEq("scene-page-0-glyphs", (long long)first.timedGlyphs, 29);
			CheckEq("scene-page-0-duration", (long long)fx::RevealDurationMs(first, 30),
				(29 - 1) * 30);
			CheckEq("scene-page-0-t0", (long long)fx::RevealAt(first, 0, 30).bytes, 3);
			const fx::Reveal half = fx::RevealAt(first, 300, 30);
			CheckEq("scene-page-0-t300", (long long)half.glyphs, 11);
			bool valid = true;
			for(int t = 0; t < 900; t++)
			{
				const fx::Reveal reveal = fx::RevealAt(first, t, 30);
				if(reveal.shown.find("\xEF\xBF\xBD") != std::string::npos)
					valid = false;
			}
			Check("scene-page-0-no-half-glyph", valid, "900 sampled times, no U+FFFD");
		}
	}
	void TestColorTone(const AstCorpusStats* ast)
	{
		printf("-- colortone (141 real uses: reset 132, sepia 9; fg style=sepia 146)\n");
		fx::ColorStyle style = fx::ColorStyle::Reset;
		Check("colortone-parse-reset", fx::ParseColorStyle("reset", style)
			&& style == fx::ColorStyle::Reset, "reset");
		Check("colortone-parse-sepia", fx::ParseColorStyle("sepia", style)
			&& style == fx::ColorStyle::Sepia, "sepia");
		Check("colortone-parse-unknown", !fx::ParseColorStyle("bleach", style),
			"an unknown style is refused, not silently linear");
		CheckStr("colortone-style-name", fx::ColorStyleName(fx::ColorStyle::Sepia), "sepia");

		// reset is the exact identity: walk every byte, all three channels
		const fx::ColorMatrix identity = fx::StyleMatrix(fx::ColorStyle::Reset);
		bool exact = true;
		for(int v = 0; v < 256; v++)
			for(int c = 0; c < 3; c++)
				if(fx::MatrixChannel(identity, c, v, v, v) != v)
					exact = false;
		Check("colortone-reset-is-exact-identity", exact, "256 values x 3 channels");

		const fx::ColorMatrix sepia = fx::StyleMatrix(fx::ColorStyle::Sepia);
		CheckEq("colortone-sepia-black-r", fx::MatrixChannel(sepia, 0, 0, 0, 0), 0);
		CheckEq("colortone-sepia-white-r", fx::MatrixChannel(sepia, 0, 255, 255, 255), 255);
		CheckEq("colortone-sepia-white-g", fx::MatrixChannel(sepia, 1, 255, 255, 255), 255);
		CheckEq("colortone-sepia-white-b", fx::MatrixChannel(sepia, 2, 255, 255, 255), 239);
		CheckEq("colortone-sepia-red-r", fx::MatrixChannel(sepia, 0, 255, 0, 0), 100);
		CheckEq("colortone-sepia-red-g", fx::MatrixChannel(sepia, 1, 255, 0, 0), 89);
		CheckEq("colortone-sepia-red-b", fx::MatrixChannel(sepia, 2, 255, 0, 0), 69);
		CheckEq("colortone-sepia-gray128-r", fx::MatrixChannel(sepia, 0, 128, 128, 128), 173);
		CheckEq("colortone-sepia-gray128-g", fx::MatrixChannel(sepia, 1, 128, 128, 128), 154);
		CheckEq("colortone-sepia-gray128-b", fx::MatrixChannel(sepia, 2, 128, 128, 128), 120);

		// clamp in both directions, and a bias that would overflow a byte
		fx::ColorMatrix over;
		over.m[0][0] = 4096; // 4x
		CheckEq("colortone-clamp-high", fx::MatrixChannel(over, 0, 255, 0, 0), 255);
		fx::ColorMatrix under;
		under.m[0][0] = -1024; // -1x
		CheckEq("colortone-clamp-low", fx::MatrixChannel(under, 0, 255, 0, 0), 0);
		fx::ColorMatrix biased;
		biased.bias[1] = 1 << 20;
		CheckEq("colortone-bias-clamped", fx::MatrixChannel(biased, 1, 0, 0, 0), 255);

		// the same call on a frame and on one layer pixel, alpha untouched
		std::vector<uint8_t> frame = { 255, 255, 255, 255, 0, 0, 0, 255, 255, 0, 0, 128,
			128, 128, 128, 255 };
		fx::ApplyColorTone(frame.data(), 4, fx::ColorStyle::Sepia);
		const uint8_t wanted[16] = { 255, 255, 239, 255, 0, 0, 0, 255, 100, 89, 69, 128,
			173, 154, 120, 255 };
		Check("colortone-frame-pixels", memcmp(frame.data(), wanted, sizeof(wanted)) == 0,
			"white/black/red/gray and alpha kept");
		std::vector<uint8_t> layer = { 255, 255, 255, 42 };
		fx::ApplyColorTone(layer.data(), 1, fx::ColorStyle::Sepia);
		Check("colortone-layer-alpha-kept", layer[0] == 255 && layer[1] == 255
			&& layer[2] == 239 && layer[3] == 42, "a 1-pixel layer takes the same path");

		// the stage-sized frame from the corpus (1024x698), timed
		const int width = 1024, height = 698;
		const size_t pixels = (size_t)width * height;
		std::vector<uint8_t> stage(pixels * 4, 128);
		for(size_t at = 3; at < stage.size(); at += 4)
			stage[at] = 255;
		const int frames = 20;
		const auto start = std::chrono::steady_clock::now();
		for(int i = 0; i < frames; i++)
			fx::ApplyColorTone(stage.data(), pixels, fx::ColorStyle::Sepia);
		const auto stop = std::chrono::steady_clock::now();
		const double perFrame = std::chrono::duration<double, std::milli>(stop - start)
			.count() / frames;
		char detail[160];
		snprintf(detail, sizeof(detail), "%.3f ms/frame (%d frames, %zu pixels)",
			perFrame, frames, pixels);
		Pass("colortone-1024x698-time", detail);
		Check("colortone-1024x698-not-pathological", perFrame < 100.0,
			"a lookup-table grade must not take seconds");

		if(ast == nullptr)
		{
			Skip("colortone-real-census", "the .ast corpus is absent");
			return;
		}
		CheckEq("colortone-real-count", ast->colortone, 141);
		CheckEq("colortone-real-reset", ast->colortoneReset, 132);
		CheckEq("colortone-real-sepia", ast->colortoneSepia, 9);
		CheckEq("colortone-real-unknown", ast->colortoneUnknown, 0);
		CheckEq("fg-style-sepia-real-count", ast->fgSepia, 146);
	}

	void TestExtrans(const AstCorpusStats* ast)
	{
		printf("-- extrans (34 real uses: time only, optional sync)\n");
		fx::ExtransTag tag;
		std::string note;
		std::vector<fx::AttrNameValue> timed = { { "time", "500" }, { "sync", "0" } };
		Check("extrans-parse-time-sync", fx::ParseExtransTag(timed, tag, note)
			&& tag.timeMs == 500 && tag.hasTime && !tag.blocksScript(),
			"time=500 sync=0 does not hold the script");
		std::vector<fx::AttrNameValue> bare;
		Check("extrans-parse-bare", fx::ParseExtransTag(bare, tag, note)
			&& !tag.hasTime && tag.timeMs == 0 && tag.blocksScript(),
			"bare tag = instant + blocking");
		std::vector<fx::AttrNameValue> bad = { { "time", "abc" } };
		Check("extrans-parse-bad-time", !fx::ParseExtransTag(bad, tag, note),
			"a non-numeric time is refused");

		CheckEq("extrans-mid-0-255", fx::MixChannel(0, 255, 500, 1000), 128);
		CheckEq("extrans-mid-10-110", fx::MixChannel(10, 110, 500, 1000), 60);
		CheckEq("extrans-start", fx::MixChannel(10, 110, 0, 1000), 10);
		CheckEq("extrans-end", fx::MixChannel(10, 110, 1000, 1000), 110);
		CheckEq("extrans-past-end", fx::MixChannel(10, 110, 5000, 1000), 110);
		CheckEq("extrans-before-start", fx::MixChannel(10, 110, -5, 1000), 10);
		CheckEq("extrans-instant", fx::MixChannel(10, 110, 0, 0), 110);
		CheckEq("extrans-weight-mid", fx::MixWeight(500, 1000), 512);

		// two 2-pixel frames: A starts, B ends, the middle is exactly the average
		const uint8_t a[8] = { 10, 20, 30, 200, 10, 20, 30, 200 };
		const uint8_t b[8] = { 110, 220, 230, 100, 110, 220, 230, 100 };
		uint8_t out[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
		fx::MixFrame(a, b, 2, 0, 1000, out);
		Check("extrans-t0-is-all-A", memcmp(out, a, sizeof(a)) == 0, "t=0");
		fx::MixFrame(a, b, 2, 500, 1000, out);
		Check("extrans-tmid-is-half", out[0] == 60 && out[1] == 120 && out[2] == 130
			&& out[3] == 150 && out[4] == 60, "A/B each half, alpha too");
		fx::MixFrame(a, b, 2, 1000, 1000, out);
		Check("extrans-tend-is-all-B", memcmp(out, b, sizeof(b)) == 0, "t=time");
		fx::MixFrame(a, b, 2, 0, 0, out);
		Check("extrans-time0-is-B", memcmp(out, b, sizeof(b)) == 0,
			"the bare tag switches at once");

		if(ast == nullptr)
		{
			Skip("extrans-real-census", "the .ast corpus is absent");
			return;
		}
		CheckEq("extrans-real-count", ast->extrans, 34);
		CheckEq("extrans-real-no-time", ast->extransNoTime, 1);
		int longest = 0, zeroes = 0;
		for(int64_t time : ast->extransTimes)
		{
			if(time > longest)
				longest = (int)time;
			if(time == 0)
				zeroes++;
		}
		CheckEq("extrans-real-longest", longest, 1000);
		CheckEq("extrans-real-zero-times", zeroes, 2); // bare tag + time=0
		CheckEq("extrans-real-sync0", ast->extransSync0, 11);
		CheckEq("extrans-real-time-500", ast->extransTimeCounts.at(500), 13);
		CheckEq("extrans-real-time-1000", ast->extransTimeCounts.at(1000), 10);
	}

	void TestExcall(const Corpus& corpus, const AstCorpusStats* ast)
	{
		printf("-- excall (56 real calls; label defaults to top; 33 targets outside this volume)\n");
		auto makeCommand = [](const char* name, std::vector<fx::AttrNameValue> attrs) {
			fx::ScriptCommand command;
			command.name = name;
			command.attrs = std::move(attrs);
			return command;
		};
		auto makeCall = [&](const char* file) {
			return makeCommand("excall", { { "file", file } });
		};
		auto makeBlock = [](const char* name, std::vector<fx::ScriptCommand> commands,
			const char* next) {
			fx::ScriptBlock block;
			block.name = name;
			block.commands = std::move(commands);
			block.next = next ? next : "";
			return block;
		};

		// synthetic A -> B -> C, with C returning explicitly and B ending its chain
		fx::ScriptProgram a;
		a.blocks.push_back(makeBlock("block_00000", { makeCommand("savetitle", {}),
			makeCall("B"), makeCommand("bg", {}) }, "block_00001"));
		a.blocks.push_back(makeBlock("block_00001", { makeCommand("se", {}) }, nullptr));
		a.labels.push_back({ "top", "block_00000" });
		fx::ScriptProgram b;
		b.blocks.push_back(makeBlock("block_00005", { makeCommand("text", {}),
			makeCall("C") }, "block_00006"));
		b.blocks.push_back(makeBlock("block_00006", { makeCommand("fg", {}) }, nullptr));
		b.labels.push_back({ "top", "block_00005" });
		fx::ScriptProgram c;
		c.blocks.push_back(makeBlock("block_00000", { makeCommand("se", {}),
			makeCommand("return", {}) }, nullptr));
		c.labels.push_back({ "top", "block_00000" });

		std::map<std::string, fx::ScriptProgram> programs = { { "A", a }, { "B", b },
			{ "C", c } };
		auto loader = [&programs](const std::string& file, fx::ScriptProgram& out,
			std::string& error) {
			const std::map<std::string, fx::ScriptProgram>::iterator found =
				programs.find(file);
			if(found == programs.end())
			{
				error = "not in the fixture";
				return false;
			}
			out = found->second;
			return true;
		};
		const fx::ExcallWalk walk = fx::WalkExcall("A", "top", loader);
		CheckEq("excall-synthetic-calls", walk.calls, 2);
		CheckEq("excall-synthetic-returns", walk.returns, 2);
		CheckEq("excall-synthetic-return-command", walk.returnCommands, 1);
		CheckEq("excall-synthetic-depth", walk.maxDepth, 2);
		CheckEq("excall-synthetic-blocks", walk.blocks, 5);
		CheckEq("excall-synthetic-commands", walk.commands, 9);
		CheckEq("excall-synthetic-unresolved", walk.unresolved, 0);
		const std::vector<std::string> wantedEvents = {
			"savetitle", "call:B:top", "text", "call:C:top", "se", "return-command",
			"return:C", "fg", "return:B", "bg", "se" };
		Check("excall-return-to-caller", walk.events == wantedEvents,
			"A calls B, B calls C, C returns to B, B returns to A");

		// a missing target is reported and the caller carries on
		fx::ScriptProgram missingProgram;
		missingProgram.blocks.push_back(makeBlock("block_00000", { makeCall("Z"),
			makeCommand("bg", {}) }, nullptr));
		std::map<std::string, fx::ScriptProgram> missingMap = { { "A2", missingProgram } };
		auto missingLoader = [&missingMap](const std::string& file, fx::ScriptProgram& out,
			std::string& error) {
			const std::map<std::string, fx::ScriptProgram>::iterator found =
				missingMap.find(file);
			if(found == missingMap.end())
			{
				error = "not in the trial volume";
				return false;
			}
			out = found->second;
			return true;
		};
		const fx::ExcallWalk missing = fx::WalkExcall("A2", "top", missingLoader);
		CheckEq("excall-missing-unresolved", missing.unresolved, 1);
		CheckEq("excall-missing-returns", missing.returns, 1);
		Check("excall-missing-list", missing.unresolvedFiles.size() == 1
			&& missing.unresolvedFiles[0] == "Z", "the target is named");
		bool continued = false;
		for(const std::string& event : missing.events)
			if(event == "bg")
				continued = true;
		Check("excall-continues-after-miss", continued, "the caller's next command runs");

		// a self-call is a cycle, not a stack overflow
		fx::ScriptProgram cyc;
		cyc.blocks.push_back(makeBlock("block_00000", { makeCall("A3"),
			makeCommand("bg", {}) }, nullptr));
		std::map<std::string, fx::ScriptProgram> cycleMap = { { "A3", cyc } };
		auto cycleLoader = [&cycleMap](const std::string& file, fx::ScriptProgram& out,
			std::string& error) {
			const std::map<std::string, fx::ScriptProgram>::iterator found =
				cycleMap.find(file);
			if(found == cycleMap.end())
			{
				error = "no";
				return false;
			}
			out = found->second;
			return true;
		};
		const fx::ExcallWalk cycle = fx::WalkExcall("A3", "top", cycleLoader);
		CheckEq("excall-cycle-counted", cycle.cycles, 1);
		CheckEq("excall-cycle-no-return", cycle.returns, 0);

		// a label that is not in the table is refused
		fx::ScriptProgram plain;
		plain.blocks.push_back(makeBlock("block_00000", { makeCommand("se", {}) },
			nullptr));
		std::map<std::string, fx::ScriptProgram> plainMap = { { "D", plain } };
		auto plainLoader = [&plainMap](const std::string& file, fx::ScriptProgram& out,
			std::string& error) {
			out = plainMap.at(file);
			return true;
		};
		const fx::ExcallWalk noLabel = fx::WalkExcall("D", "nope", plainLoader);
		CheckEq("excall-missing-label", noLabel.missingLabels, 1);
		CheckEq("excall-missing-label-blocks", noLabel.blocks, 0);

		if(ast == nullptr || corpus.astDir.empty())
		{
			Skip("excall-real-chain", "the .ast corpus is absent");
			return;
		}
		auto realLoader = [&corpus](const std::string& file, fx::ScriptProgram& out,
			std::string& error) {
			return LoadAstProgram(corpus.astDir, file, out, error);
		};
		// the longest real chain in the trial: six calls, all inside this volume
		const fx::ExcallWalk deep = fx::WalkExcall("d09_hy01_1", "top", realLoader);
		CheckEq("excall-real-depth-6", deep.maxDepth, 6);
		CheckEq("excall-real-deep-calls", deep.calls, 6);
		CheckEq("excall-real-deep-returns", deep.returns, 6);
		CheckEq("excall-real-deep-unresolved", deep.unresolved, 0);
		bool reachedHy03 = false;
		for(const std::string& event : deep.events)
			if(event == "call:d09_hy03:top")
				reachedHy03 = true;
		Check("excall-real-deep-last-call", reachedHy03, "d09_hy01_1 -> ... -> d09_hy03");
		printf("  d09_hy01_1: calls %d, returns %d, depth %d, blocks %d, commands %d\n",
			deep.calls, deep.returns, deep.maxDepth, deep.blocks, deep.commands);
		for(const std::string& event : deep.events)
			if(event.rfind("call:", 0) == 0)
				printf("    event %s\n", event.c_str());

		// the nested pair the lead asked about, and its first missing volume target
		const fx::ExcallWalk nested = fx::WalkExcall("d01_c01_3", "top", realLoader);
		bool called4 = false, returned4 = false;
		for(const std::string& event : nested.events)
		{
			if(event == "call:d01_c01_4:top")
				called4 = true;
			if(event == "return:d01_c01_4")
				returned4 = true;
		}
		Check("excall-real-nested-call", called4 && returned4,
			"d01_c01_3 -> d01_c01_4 -> returns");
		CheckEq("excall-real-nested-calls", nested.calls, 3);
		CheckEq("excall-real-nested-unresolved", nested.unresolved, 1);
		Check("excall-real-missing-volume",
			nested.unresolvedFiles.size() == 1 && nested.unresolvedFiles[0] == "d01_c02_1",
			"the missing target is the next volume's script");
		printf("  d01_c01_3: calls %d, depth %d, unresolved %d (%s)\n", nested.calls,
			nested.maxDepth, nested.unresolved,
			nested.unresolvedFiles.empty() ? "-" : nested.unresolvedFiles[0].c_str());
		for(const std::string& event : nested.events)
			if(event.rfind("call:", 0) == 0 || event.rfind("return:", 0) == 0)
				printf("    event %s\n", event.c_str());

		CheckEq("excall-real-count", ast->excall, 56);
		CheckEq("excall-real-unique-targets", (int)ast->excallTargets.size(), 56);
		CheckEq("excall-real-label-attribute", ast->excallWithLabel, 0);
		int present = 0;
		for(const std::string& target : ast->excallTargets)
			if(IsFile(corpus.astDir + "/" + target + ".ast"))
				present++;
		CheckEq("excall-real-targets-present", present, 23);
		CheckEq("excall-real-targets-missing", (int)ast->excallTargets.size() - present, 33);
		CheckEq("excall-real-label-tables", ast->labelsTop, 59);
		printf("  corpus: 56 calls / 56 targets; %d resolved, %d outside this volume\n",
			present, (int)ast->excallTargets.size() - present);
		printf("  first missing:");
		int shown = 0;
		for(const std::string& target : ast->excallTargets)
			if(!IsFile(corpus.astDir + "/" + target + ".ast") && shown++ < 6)
				printf(" %s", target.c_str());
		printf("\n");
	}
	void TestTags(const Corpus& corpus, const AstCorpusStats* ast)
	{
		printf("-- tags (cacheclear 292, exfont 266, savetitle 55, movie 5, ...)\n");
		const char* names[] = { "movie", "タイトル", "autoplay", "skipstop",
			"cacheclear", "exfont", "savetitle" };
		bool allRegistered = true;
		for(const char* name : names)
			if(fx::FindTag(name) == nullptr)
				allRegistered = false;
		Check("tags-registry", allRegistered, "all seven real names are registered");
		Check("tags-unknown", fx::FindTag("excall") == nullptr,
			"a tag from another module is not in this registry");
		const fx::TagSpec* movieSpec = fx::FindTag("movie");
		Check("tags-movie-not-playable", movieSpec != nullptr
			&& movieSpec->status == fx::TagStatus::NotPlayable,
			"movie is known and says playback is not implemented");
		Check("tags-cacheclear-implemented", fx::FindTag("cacheclear") != nullptr
			&& fx::FindTag("cacheclear")->status == fx::TagStatus::Implemented,
			"cacheclear does the work here");
		Check("tags-savetitle-implemented", fx::FindTag("savetitle") != nullptr
			&& fx::FindTag("savetitle")->status == fx::TagStatus::Implemented,
			"savetitle does the work here");

		fx::MovieTag movie;
		std::string note;
		std::vector<fx::AttrNameValue> movieAttrs = { { "file", "cutin_rin" },
			{ "cancel", "1" }, { "bg", "black" } };
		Check("movie-parse", fx::ParseMovieTag(movieAttrs, movie, note)
			&& movie.file == "cutin_rin" && movie.cancel && movie.bg == "black"
			&& !movie.playbackImplemented(), note);
		std::vector<fx::AttrNameValue> noFile;
		Check("movie-without-file", !fx::ParseMovieTag(noFile, movie, note),
			"a movie tag with no path is refused");

		fx::AutoplayTag autoplay;
		std::vector<fx::AttrNameValue> stopAttrs = { { "mode", "stop" } };
		Check("autoplay-stop", fx::ParseAutoplayTag(stopAttrs, autoplay, note)
			&& autoplay.stop && autoplay.hasMode, "mode=stop");
		std::vector<fx::AttrNameValue> startAttrs;
		Check("autoplay-start", fx::ParseAutoplayTag(startAttrs, autoplay, note)
			&& !autoplay.stop && !autoplay.hasMode, "bare = start");

		fx::ExfontTag exfont;
		std::vector<fx::AttrNameValue> f4 = { { "size", "f4" } };
		Check("exfont-size", fx::ParseExfontTag(f4, exfont, note) && exfont.hasSize
			&& exfont.size == "f4", "size=f4");
		std::vector<fx::AttrNameValue> bareFont;
		Check("exfont-default", fx::ParseExfontTag(bareFont, exfont, note)
			&& !exfont.hasSize, "no size = default face");

		// UTF-8 safety: a byte cut must never become half a glyph
		Check("utf8-valid-japanese", fx::IsValidUtf8("監獄の詩"), "three-byte glyphs");
		Check("utf8-invalid-partial", !fx::IsValidUtf8(std::string("\xE3\x81", 2)),
			"two bytes of a three-byte glyph are not UTF-8");
		const std::string japanese = "あいう"; // 9 bytes
		CheckStr("utf8-truncate-4", fx::TruncateUtf8(japanese, 4), "あ");
		CheckStr("utf8-truncate-3", fx::TruncateUtf8(japanese, 3), "あ");
		CheckStr("utf8-truncate-2", fx::TruncateUtf8(japanese, 2), "");
		CheckStr("utf8-truncate-ascii", fx::TruncateUtf8("ASCII title", 5), "ASCII");
		Check("utf8-truncate-stays-valid", fx::IsValidUtf8(fx::TruncateUtf8("監獄の詩", 7)),
			"a truncated title is still UTF-8");

		if(ast != nullptr && ast->savetitle > 0)
		{
			fx::SaveTitleBook book;
			int rejected = 0;
			for(const std::string& title : ast->savetitleAll)
				if(!book.Set(title))
					rejected++;
			CheckEq("savetitle-real-sets", (long long)book.Sets(), 55);
			CheckEq("savetitle-real-unique", (long long)book.UniqueTitles(), 24);
			CheckEq("savetitle-real-rejected", rejected, 0);
			bool safe = true;
			for(const std::string& title : book.History())
				if(!fx::IsValidUtf8(title) || title.size() > fx::SaveTitleBook::kMaxTitleBytes)
					safe = false;
			Check("savetitle-store-is-utf8-safe", safe,
				"55 real titles, every stored byte inside 96 and on a boundary");
			Check("savetitle-current", !book.Current().empty(),
				"the last real title becomes current");
		}
		else
			Skip("savetitle-real-sets", "the .ast corpus is absent");

		// cacheclear is a real store, not a counter
		fx::TextureCache cache;
		cache.Insert("bg", 100);
		cache.Insert("fg", 250);
		cache.Insert("bg", 300); // same name: replaced, not added twice
		CheckEq("cacheclear-count", (long long)cache.Count(), 2);
		CheckEq("cacheclear-bytes", (long long)cache.Bytes(), 550);
		const fx::TextureCache::ClearResult freed = cache.Clear();
		CheckEq("cacheclear-freed-textures", (long long)freed.textures, 2);
		CheckEq("cacheclear-freed-bytes", (long long)freed.bytes, 550);
		CheckEq("cacheclear-empty", (long long)cache.Count(), 0);
		const fx::TextureCache::ClearResult again = cache.Clear();
		CheckEq("cacheclear-twice", (long long)again.textures, 0);

		if(ast == nullptr)
		{
			Skip("tags-real-census", "the .ast corpus is absent");
			return;
		}
		CheckEq("tags-files", ast->files, 59);
		CheckEq("tags-parse-failures", ast->parseFailures, 0);
		CheckEq("tags-labels-top", ast->labelsTop, 59);
		Check("tags-labels-other", ast->labelsOther >= 1,
			"at least one script has a second label");
		CheckEq("tags-cacheclear-real", ast->cacheclear, 292);
		CheckEq("tags-exfont-real", ast->exfont, 266);
		Check("tags-exfont-sized", ast->exfontSized > 0 && ast->exfontSized < ast->exfont,
			"some tags carry size=f4, most are bare");
		CheckEq("tags-title-real", ast->titleTag, 2);
		CheckEq("tags-autoplay-real", ast->autoplay, 2);
		CheckEq("tags-autoplay-stop-real", ast->autoplayStop, 1);
		CheckEq("tags-skipstop-real", ast->skipstop, 1);
		CheckEq("tags-movie-real", ast->movie, 5);
		CheckEq("tags-movie-cancel-real", ast->movieCancel, 5);
		CheckEq("tags-movie-black-real", ast->movieBlackBg, 3);
		CheckEq("tags-savetitle-real", ast->savetitle, 55);
		CheckEq("tags-savetitle-unique", (long long)ast->savetitleTexts.size(), 24);
		bool hasMovieFiles = true;
		const char* wantedFiles[] = { "op", "ed2", "cutin_shuichiro", "cutin_ozawa",
			"cutin_rin" };
		for(const char* wanted : wantedFiles)
		{
			bool found = false;
			for(const fx::MovieTag& tag : ast->movies)
				if(tag.file == wanted)
					found = true;
			if(!found)
				hasMovieFiles = false;
		}
		Check("movie-real-paths", hasMovieFiles,
			"op / ed2 / three cutins, all cancel=1");
		printf("  census: cacheclear %d, exfont %d (sized %d), savetitle %d (%zu distinct), "
			"movie %d, タイトル %d, autoplay %d, skipstop %d\n", ast->cacheclear,
			ast->exfont, ast->exfontSized, ast->savetitle, ast->savetitleTexts.size(),
			ast->movie, ast->titleTag, ast->autoplay, ast->skipstop);
	}

	void TestGuiState(const Corpus& corpus)
	{
		printf("-- scetween state slots (16: 8 real, 8 inferred)\n");
		fx::GuiStateMap map = fx::GuiStateMap::Default();
		CheckEq("guistate-slot-count", (long long)map.Slots().size(), 16);
		CheckEq("guistate-observed-slots", (long long)map.ObservedSlots(), 8);
		CheckEq("guistate-inferred-slots", (long long)map.InferredSlots(), 8);
		std::set<std::string> states;
		for(const fx::GuiSlot& slot : map.Slots())
			states.insert(slot.state);
		CheckEq("guistate-unique-states", (long long)states.size(), 16);
		Check("guistate-all-have-layers", [&map]() {
			for(const fx::GuiSlot& slot : map.Slots())
				if(slot.layer.empty() || slot.properties.empty())
					return false;
			return true;
		}(), "every slot resolves to a layer role and a property");

		const fx::GuiSlot* in = map.Find("in");
		const fx::GuiProperty* inAlpha = in ? in->Property("alpha") : nullptr;
		Check("guistate-in", in != nullptr && in->layer == "msg.text" && in->observed
			&& inAlpha != nullptr && inAlpha->init == 255 && inAlpha->diff == -255
			&& inAlpha->target == 0, "in: msg.text alpha 255 -> 0");
		const fx::GuiSlot* out = map.Find("out");
		const fx::GuiProperty* outAlpha = out ? out->Property("alpha") : nullptr;
		Check("guistate-out", outAlpha != nullptr && outAlpha->init == 0
			&& outAlpha->diff == -255 && outAlpha->target == 0, "out: alpha 0 -> 0");
		const fx::GuiSlot* downIn = map.Find("backlog_down_in");
		const fx::GuiProperty* downTop = downIn ? downIn->Property("top") : nullptr;
		Check("guistate-backlog-down-in", downTop != nullptr && downTop->init == 0
			&& downTop->diff == -288 && downTop->target == -288,
			"backlog_down_in: top 0 -> -288");
		const fx::GuiSlot* upIn = map.Find("backlog_up_in");
		const fx::GuiProperty* upTop = upIn ? upIn->Property("top") : nullptr;
		Check("guistate-backlog-up-in", upTop != nullptr && upTop->diff == 288
			&& upTop->target == 288, "backlog_up_in: top 0 -> +288");
		const fx::GuiSlot* rollover = map.Find("rollover");
		const fx::GuiProperty* rolloverAlpha = rollover
			? rollover->Property("alpha") : nullptr;
		Check("guistate-inferred-rollover", rollover != nullptr && !rollover->observed
			&& rolloverAlpha != nullptr && rolloverAlpha->target == 255,
			"rollover: inferred, hover layer 0 -> 255");

		if(!corpus.first.empty())
		{
			GuiScriptStats stats;
			std::string error;
			if(ScanGuiScript(corpus.first, stats, error))
			{
				CheckEq("guistate-real-definitions", stats.total, 80);
				CheckEq("guistate-real-commented", stats.commented, 8);
				CheckEq("guistate-real-init", stats.init, 32);
				CheckEq("guistate-real-add", stats.add, 48);
				CheckEq("guistate-real-types", (long long)stats.types.size(), 8);
				CheckEq("guistate-real-msec", stats.msecSpelling, 2);
				Check("guistate-real-time", stats.timeSpelling > 0, "time= is the spelling");
				bool fourContexts = true;
				for(const auto& entry : stats.signatureCounts)
					if(entry.second != 4)
						fourContexts = false;
				Check("guistate-four-contexts", fourContexts,
					"every active signature repeats in four [chgmsg] contexts");
				int applied = 0, unknown = 0;
				for(const fx::GuiDefinition& definition : stats.definitions)
				{
					if(map.Apply(definition))
						applied++;
					else
						unknown++;
				}
				CheckEq("guistate-applied", applied, 80);
				CheckEq("guistate-unknown-types", unknown, 0);
				CheckEq("guistate-observed-after-apply",
					(long long)map.ObservedSlots(), 8);
				const fx::GuiSlot* appliedIn = map.Find("in");
				const fx::GuiProperty* appliedAlpha = appliedIn
					? appliedIn->Property("alpha") : nullptr;
				Check("guistate-apply-agrees", appliedAlpha != nullptr
					&& appliedAlpha->init == 255 && appliedAlpha->diff == -255
					&& appliedAlpha->target == 0, "parser and default table agree");
			}
			else
				Check("guistate-real-definitions", false, error);
		}
		else
			Skip("guistate-real-definitions", "first.iet is absent");

		if(!corpus.macroEffect.empty())
		{
			GuiScriptStats stats;
			std::string error;
			if(ScanGuiScript(corpus.macroEffect, stats, error))
			{
				CheckEq("guistate-macro-definitions", stats.total, 8);
				CheckEq("guistate-macro-msec-spelling", stats.msecSpelling, 4);
				CheckEq("guistate-macro-time-spelling", stats.timeSpelling, 0);
				int msecMs = 0;
				for(const fx::GuiDefinition& definition : stats.definitions)
					if(definition.timeSpelling == "msec" && definition.timeMs == 400)
						msecMs++;
				CheckEq("guistate-macro-msec-400", msecMs, 4);
			}
			else
				Check("guistate-macro-definitions", false, error);
		}
		else
			Skip("guistate-macro-definitions", "macro_effect.iet is absent");

		// the table is configurable: a title with different layer names overrides one
		Check("guistate-config-slot", map.SetSlot("in", "custom.msg", "override"),
			"retarget in to custom.msg");
		Check("guistate-config-property", map.SetProperty("in", "alpha", 255, 128),
			"override the alpha delta");
		const fx::GuiSlot* custom = map.Find("in");
		const fx::GuiProperty* customAlpha = custom ? custom->Property("alpha") : nullptr;
		Check("guistate-config-applied", custom != nullptr && custom->layer == "custom.msg"
			&& customAlpha != nullptr && customAlpha->target == 255,
			"255+128 clamps to 255");

		printf("  %-18s %-20s %-34s %s\n", "slot", "layer role", "property init->target",
			"source");
		for(const fx::GuiSlot& slot : map.Slots())
		{
			std::string properties;
			for(const fx::GuiProperty& property : slot.properties)
			{
				if(!properties.empty())
					properties += ", ";
				properties += property.param + " " + std::to_string(property.init) + "->"
					+ std::to_string(property.target);
			}
			printf("  %-18s %-20s %-34s %s\n", slot.state.c_str(), slot.layer.c_str(),
				properties.c_str(), slot.observed ? "real" : "inferred");
		}
	}
}

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		printf("usage: fxtool <pages|typewriter|tween|scetween|anime|sfade|quake|"
			"colortone|extrans|excall|tags|guistate|selftest> ...\n");
		printf("       fxtool colortone <in.png> <out.png> [--style sepia]\n");
		printf("       fxtool colortone --bench [--count N] [--style sepia]\n");
		printf("       fxtool extrans <a.png> <b.png> <out.png> [--time ms] [--at ms]\n");
		printf("       fxtool excall [file]\n");
		printf("       fxtool tags [ast-directory]\n");
		printf("       fxtool guistate [first.iet]\n");
		printf("       fxtool selftest\n");
		return 2;
	}
	const std::string command = argv[1];
	if(command == "selftest")
		return RunSelfTest();

	Options options;
	std::vector<std::string> positional;
	for(int i = 2; i < argc; i++)
	{
		const std::string arg = argv[i];
		const bool hasNext = i + 1 < argc;
		auto next = [&]() -> std::string {
			return hasNext ? std::string(argv[++i]) : std::string();
		};
		if(arg == "--page" && hasNext)
			options.page = atoi(next().c_str());
		else if(arg == "--at" && hasNext)
		{
			if(!ParseTimes(next(), options.at))
			{
				fprintf(stderr, "fxtool: --at wants a comma list of milliseconds\n");
				return 2;
			}
		}
		else if((arg == "--from" || arg == "--base" || arg == "--size" || arg == "--gain") &&
			hasNext)
			options.from = atoll(next().c_str());
		else if((arg == "--to" || arg == "--diff") && hasNext)
			options.to = atoll(next().c_str());
		else if((arg == "--ms" || arg == "--time") && hasNext)
			options.ms = atoll(next().c_str());
		else if(arg == "--delay" && hasNext)
			options.delay = atoll(next().c_str());
		else if(arg == "--yoyo" && hasNext)
			options.yoyo = atoi(next().c_str());
		else if(arg == "--seed" && hasNext)
			options.seed = strtoull(next().c_str(), nullptr, 10);
		else if((arg == "--count" || arg == "--limit") && hasNext)
			options.count = atoi(next().c_str());
		else if(arg == "--msgspeed" && hasNext)
			options.msgspeed = atoi(next().c_str());
		else if(arg == "--ease" && hasNext)
			options.ease = next();
		else if(arg == "--id" && hasNext)
			options.id = next();
		else if(arg == "--style" && hasNext)
			options.style = next();
		else if(arg == "--bench")
			options.bench = true;
		else if(arg == "--dir" && hasNext)
			options.vertical = next() == "v";
		else if(!arg.empty() && arg[0] == '-')
		{
			fprintf(stderr, "fxtool: unknown option %s\n", arg.c_str());
			return 2;
		}
		else
			positional.push_back(arg);
	}

	if(command == "pages")
	{
		if(positional.empty())
		{
			fprintf(stderr, "fxtool: pages needs a script\n");
			return 2;
		}
		return CommandPages(positional[0], options.count);
	}
	if(command == "typewriter")
	{
		if(positional.empty())
		{
			fprintf(stderr, "fxtool: typewriter needs a script\n");
			return 2;
		}
		return CommandTypewriter(positional[0], options);
	}
	if(command == "tween")
		return CommandTween(options);
	if(command == "scetween")
		return CommandScetween(options);
	if(command == "lytween")
	{
		if(positional.empty())
		{
			fprintf(stderr, "fxtool: lytween needs a script\n");
			return 2;
		}
		return CommandLytween(positional[0]);
	}
	if(command == "anime")
	{
		if(positional.empty())
		{
			fprintf(stderr, "fxtool: anime needs an .ipt or an .iet\n");
			return 2;
		}
		return CommandAnime(positional[0], options);
	}
	if(command == "sfade")
	{
		if(positional.size() < 2)
		{
			fprintf(stderr, "fxtool: sfade needs <in.png> <out.png>\n");
			return 2;
		}
		return CommandSfade(positional[0], positional[1], options);
	}
	if(command == "quake")
		return CommandQuake(options);
	if(command == "colortone")
		return CommandColorTone(options, positional);
	if(command == "extrans")
		return CommandExtrans(options, positional);
	if(command == "excall")
		return CommandExcall(positional);
	if(command == "tags")
		return CommandTags(positional);
	if(command == "guistate")
		return CommandGuistate(positional);

	fprintf(stderr, "fxtool: unknown command %s\n", command.c_str());
	return 2;
}
