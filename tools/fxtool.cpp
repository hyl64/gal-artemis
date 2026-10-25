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

#include "iet.h"
#include "ipt.h"
#include "pngio.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
	// selftest
	// ---------------------------------------------------------------------------
	void TestTween();
	void TestTypewriter();
	void TestQuake();
	void TestBrightness(const Corpus& corpus);
	void TestLytween(const Corpus& corpus);
	void TestAnimation(const Corpus& corpus);
	void TestPages(const Corpus& corpus);

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
}

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		printf("usage: fxtool <pages|typewriter|tween|scetween|anime|sfade|quake|selftest> ...\n");
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

	fprintf(stderr, "fxtool: unknown command %s\n", command.c_str());
	return 2;
}
