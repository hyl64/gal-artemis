// agame - play a scenario on a window
//
// Two front ends, one picture:
//
//   agame <script.ast> [--frames <dir>]
//       the original demonstration: the engine's own action stream from a
//       scenario this tree made, twelve frames, a gradient sky.  Kept exactly
//       as it was, because build-all.sh and the older notes run it.
//
//   agame --script <scene.iet> --archive <root.pfs> [--data-dir <dir>]
//         [--boot first.iet] [--frames <dir>] [--out <png>] [--max-pages N]
//       the real thing: the game's own archive, the game's own boot script, the
//       game's own macro files and the game's own TTF, composited on the stage
//       size the game's own system.ini asks for.  Headless with
//       SDL_VIDEODRIVER=dummy; no window is opened on this path at all.
//
//   agame selftest
//       the checks, including the real-data ones.  A missing corpus is a SKIP,
//       not a failure: the test belongs in the tree, the game does not.
//
// The first screen is not "declared" to exist: [rp] hands the driver a page, the
// driver draws the same picture twice (with and without the words), and the
// difference between the two framebuffers is the number of text pixels.  The PNG
// that lands is then read back through pngio and measured again.

#include "../src/runtime.h"

#include "../src/iet.h"
#include "../src/render/astscene.h"
#include "../src/render/play/play.h"
#include "../src/render/screen.h"
#include "../src/render/vm.h"

#include <SDL.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>
#include <unordered_set>
#include <vector>

// ===========================================================================
// the original .ast player
// ===========================================================================

// cut a UTF-8 string after `count` characters, never inside one
static std::string FirstChars(const std::string& s, size_t count)
{
	size_t at = 0, seen = 0;
	while(at < s.size() && seen < count)
	{
		const unsigned char c = (unsigned char)s[at];
		size_t len = 1;
		if((c & 0x80) == 0) len = 1;
		else if((c & 0xE0) == 0xC0) len = 2;
		else if((c & 0xF0) == 0xE0) len = 3;
		else if((c & 0xF8) == 0xF0) len = 4;
		at += len;
		seen++;
	}
	return s.substr(0, at);
}

static std::string ReadFile(const char* path)
{
	FILE* f = fopen(path, "rb");
	if(f == nullptr)
		return std::string();
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::string out((size_t)(size > 0 ? size : 0), '\0');
	if(size > 0)
		out.resize(fread(&out[0], 1, out.size(), f));
	fclose(f);
	return out;
}

struct LineBitmap
{
	// RGBA, one byte each, in that order: the context below is created with a
	// byte-ordered colour space so the layout is not something to guess at
	std::vector<uint8_t> pixels;
	int width = 0;
	int height = 0;
};

static CTFontRef gFont = nullptr;

static bool InitFont(float size)
{
	// the first of these that exists is used; Hiragino is on every macOS
	const char* names[] = { "HiraginoSans-W3", "HiraKakuProN-W3", "Helvetica" };
	for(const char* name : names)
	{
		CFStringRef cfname = CFStringCreateWithCString(kCFAllocatorDefault, name, kCFStringEncodingUTF8);
		gFont = CTFontCreateWithName(cfname, size, nullptr);
		CFRelease(cfname);
		if(gFont != nullptr)
			return true;
	}
	return false;
}

static LineBitmap RenderLine(const std::string& text, float size)
{
	LineBitmap out;
	if(gFont == nullptr)
	{
		InitFont(size);
		if(getenv("AGAME_DEBUG") != nullptr)
		{
			CFStringRef ps = CTFontCopyPostScriptName(gFont);
			char name[128] = { 0 };
			CFStringGetCString(ps, name, sizeof name, kCFStringEncodingUTF8);
			CFRelease(ps);
			fprintf(stderr, "[font] %s at %.1fpt (asked %.1f)\n", name, CTFontGetSize(gFont),
				(double)size);
			fflush(stderr);
		}
	}
	if(gFont == nullptr)
		return out;

	CFStringRef cftext = CFStringCreateWithCString(kCFAllocatorDefault, text.c_str(), kCFStringEncodingUTF8);
	if(cftext == nullptr)
		return out;

	// CTLineDraw takes the colour from the attributes, not from the context's fill
	// colour: without this the text is black on a dark box
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	const CGFloat white[] = { 1.0, 1.0, 1.0, 1.0 };
	CGColorRef whiteColour = CGColorCreate(rgb, white);
	CGColorSpaceRelease(rgb);
	// CoreText takes everything from the attributes: the colour *and* the font.
	// Without the font attribute it falls back to a 12pt default, which is why the
	// first version drew the right characters at the wrong size.
	const void* keys[] = { kCTForegroundColorAttributeName, kCTFontAttributeName };
	const void* values[] = { whiteColour, gFont };
	CFDictionaryRef attributes = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
		&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFAttributedStringRef attr = CFAttributedStringCreate(kCFAllocatorDefault, cftext, attributes);
	CTLineRef line = CTLineCreateWithAttributedString(attr);
	CGFloat ascent = 0, descent = 0, leading = 0;
	const double width = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
	if(getenv("AGAME_DEBUG") != nullptr)
	{
		fprintf(stderr, "[line] chars=%ld width=%.1f ascent=%.1f descent=%.1f leading=%.1f\n",
			(long)CFStringGetLength(cftext), width, (double)ascent, (double)descent, (double)leading);
		fflush(stderr);
	}

	const int w = (int)width + 8;
	const int h = (int)(ascent + descent + leading) + 8;
	if(w <= 8 || h <= 8)
	{
		CFRelease(line);
		CFRelease(attr);
		CFRelease(attributes);
		CFRelease(cftext);
		return out;
	}

	std::vector<uint8_t> buffer((size_t)w * h * 4, 0);
	CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
	CGContextRef ctx = CGBitmapContextCreate(buffer.data(), w, h, 8, (size_t)w * 4, space,
		kCGImageAlphaPremultipliedLast);
	CGColorSpaceRelease(space);
	if(ctx == nullptr)
	{
		CFRelease(line);
		CFRelease(attr);
		CFRelease(attributes);
		CFRelease(cftext);
		return out;
	}

	CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
	CGContextSetTextPosition(ctx, 4, 4 + descent);
	CTLineDraw(line, ctx);
	CGContextRelease(ctx);

	CGColorRelease(whiteColour);
	out.pixels = std::move(buffer);
	out.width = w;
	out.height = h;

	CFRelease(line);
	CFRelease(attr);
	CFRelease(attributes);
	CFRelease(cftext);
	return out;
}

struct Screen
{
	SDL_Window* window = nullptr;
	SDL_Renderer* renderer = nullptr;
	SDL_Texture* texture = nullptr;
	std::vector<uint32_t> pixels;
	int width = 1280;
	int height = 720;

	bool Open()
	{
		if(SDL_Init(SDL_INIT_VIDEO) != 0)
			return false;
		window = SDL_CreateWindow("Artemis", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
			width, height, 0);
		if(window == nullptr)
			return false;
		renderer = SDL_CreateRenderer(window, -1, 0);
		texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
			SDL_TEXTUREACCESS_STREAMING, width, height);
		pixels.assign((size_t)width * height, 0);
		return texture != nullptr;
	}

	void Background()
	{
		// a night sky: vertical gradient from deep blue to almost black
		for(int y = 0; y < height; y++)
		{
			const int shade = 30 - y * 18 / height;
			const uint32_t colour = 0xff000000u | (uint32_t)(shade / 2) << 16
				| (uint32_t)(shade) << 8 | (uint32_t)(shade * 2);
			for(int x = 0; x < width; x++)
				pixels[(size_t)y * width + x] = colour;
		}
	}

	void Box(int x, int y, int w, int h, uint32_t colour)
	{
		for(int yy = y; yy < y + h; yy++)
		{
			if(yy < 0 || yy >= height)
				continue;
			for(int xx = x; xx < x + w; xx++)
				if(xx >= 0 && xx < width)
					pixels[(size_t)yy * width + xx] = colour;
		}
	}

	void Blit(const LineBitmap& line, int x, int y)
	{
		if(getenv("AGAME_DEBUG") != nullptr)
		{
			fprintf(stderr, "[blit] at %d,%d size %dx%d (screen %dx%d)\n", x, y,
				line.width, line.height, width, height);
			fflush(stderr);
		}
		for(int yy = 0; yy < line.height; yy++)
		{
			if(y + yy < 0 || y + yy >= height)
				continue;
			for(int xx = 0; xx < line.width; xx++)
			{
				if(x + xx < 0 || x + xx >= width)
					continue;
				const uint8_t* src = &line.pixels[((size_t)yy * line.width + xx) * 4];
				const uint32_t a = src[3];
				if(a == 0)
					continue;
				const size_t at = (size_t)(y + yy) * width + (x + xx);
				// the text is white, so the blend is the coverage
				const uint32_t dst = pixels[at];
				const uint32_t r = (src[0] * a + ((dst >> 16) & 0xff) * (255 - a)) / 255;
				const uint32_t g = (src[1] * a + ((dst >> 8) & 0xff) * (255 - a)) / 255;
				const uint32_t b = (src[2] * a + (dst & 0xff) * (255 - a)) / 255;
				pixels[at] = 0xff000000u | r << 16 | g << 8 | b;
			}
		}
	}

	void Present()
	{
		SDL_UpdateTexture(texture, nullptr, pixels.data(), width * 4);
		SDL_RenderClear(renderer);
		SDL_RenderCopy(renderer, texture, nullptr, nullptr);
		SDL_RenderPresent(renderer);
	}

	bool Save(const std::string& path)
	{
		SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), width, height, 32,
			width * 4, SDL_PIXELFORMAT_ARGB8888);
		const bool ok = surf != nullptr && SDL_SaveBMP(surf, path.c_str()) == 0;
		if(surf != nullptr)
			SDL_FreeSurface(surf);
		return ok;
	}
};

static int RunAst(const std::string& scriptPath, const std::string& framesDir,
	const std::string& outPath)
{
	const bool headless = !framesDir.empty();
	if(headless && mkdir(framesDir.c_str(), 0755) != 0 && errno != EEXIST)
	{
		fprintf(stderr, "agame: cannot create frame directory %s\n", framesDir.c_str());
		return 1;
	}

	std::string source = ReadFile(scriptPath.c_str());
	if(source.empty())
	{
		fprintf(stderr, "agame: cannot read %s\n", scriptPath.c_str());
		return 1;
	}

	artemis::Scenario scenario(source);
	Screen screen;
	if(!screen.Open())
	{
		fprintf(stderr, "agame: cannot open a window (%s)\n", SDL_GetError());
		return 1;
	}

	int frame = 0;
	bool running = true;
	int64_t waitUntil = 0; // a pause marker
	std::string line;
	bool haveLine = false;

	while(running)
	{
		artemis::Action action = scenario.Next();
		switch(action.type)
		{
		case artemis::ActionType::Line:
			line = action.text;
			haveLine = true;
			break;
		case artemis::ActionType::Wait:
			waitUntil = SDL_GetTicks64() + (Uint64)action.value;
			continue; // a pause changes nothing on screen
		case artemis::ActionType::Jump:
			continue;
		case artemis::ActionType::Unknown:
			fprintf(stderr, "agame: the runtime does not understand %s\n", action.text.c_str());
			continue;
		case artemis::ActionType::End:
			running = false;
			continue;
		}

		// reveal the line a character at a time: the frame drawn is the part of it
		// that has been "typed" so far
		static std::string shown;
		shown.clear();

		// draw the frame
		screen.Background();
		screen.Box(60, screen.height - 260, screen.width - 120, 200, 0xb0101418u);
		if(haveLine)
		{
			if(headless)
			{
				// a few points of the reveal, so the effect shows up in the frames
				static const int stages[] = {1, 3, 1000000};
				for(int stage : stages)
				{
					const std::string partialText = stage >= 1000 ? line : FirstChars(line, (size_t)stage);
					LineBitmap partial = RenderLine(partialText, 42.0f);
					screen.Background();
					screen.Box(60, screen.height - 260, screen.width - 120, 200, 0xb0101418u);
					screen.Blit(partial, 96, screen.height - 220);
					screen.Present();
					char path[512];
					snprintf(path, sizeof path, "%s/frame%03d.bmp", framesDir.c_str(), frame);
					if(!screen.Save(path))
					{
						fprintf(stderr, "agame: cannot write %s\n", path);
						return 1;
					}
					printf("frame %d: %s\n", frame, partialText.c_str());
					frame++;
				}
				continue;
			}
			LineBitmap rendered = RenderLine(line, 42.0f);
			if(getenv("AGAME_DEBUG") != nullptr)
			{
				int maxA = 0, inked = 0;
				for(size_t i = 3; i < rendered.pixels.size(); i += 4)
				{
					const int a = rendered.pixels[i];
					if(a > maxA) maxA = a;
					if(a != 0) inked++;
				}
				// show the first pixel that carries any ink, all four bytes, so the
				// layout is known rather than assumed
				for(size_t i = 0; i + 3 < rendered.pixels.size(); i += 4)
					if(rendered.pixels[i] || rendered.pixels[i+1] || rendered.pixels[i+2]
						|| rendered.pixels[i+3])
					{
						fprintf(stderr, "[text px] i=%zu bytes=%u,%u,%u,%u\n", i,
							rendered.pixels[i], rendered.pixels[i+1], rendered.pixels[i+2],
							rendered.pixels[i+3]);
						break;
					}
				fprintf(stderr, "[text] \"%s\" -> %dx%d maxAlpha=%d inked=%d\n",
					line.c_str(), rendered.width, rendered.height, maxA, inked);
				fflush(stderr);
			}
			screen.Blit(rendered, 96, screen.height - 220);
		}
		screen.Present();

		if(getenv("AGAME_DEBUG") != nullptr)
		{
			uint32_t mx = 0;
			for(int yy = screen.height - 240; yy < screen.height - 180; yy++)
				for(int xx = 90; xx < 400; xx++)
				{
					const uint32_t px = screen.pixels[(size_t)yy * screen.width + xx];
					const uint32_t v = px & 0xff;
					if(v > mx) mx = v;
				}
			fprintf(stderr, "[frame] max blue in the text band: %u, at(100,505)=%08x\n", mx,
				screen.pixels[(size_t)505 * screen.width + 100]);
			fflush(stderr);
		}
		if(headless)
		{
			char path[512];
			snprintf(path, sizeof path, "%s/frame%03d.bmp", framesDir.c_str(), frame);
			if(!screen.Save(path))
				fprintf(stderr, "agame: cannot write %s\n", path);
			printf("frame %d: %s\n", frame, line.c_str());
			frame++;
			continue;
		}

		// wait for the reader, honouring any pause first
		while(SDL_GetTicks64() < (Uint64)waitUntil)
			SDL_Delay(10);
		bool advance = false;
		while(!advance)
		{
			SDL_Event e;
			while(SDL_PollEvent(&e))
			{
				if(e.type == SDL_QUIT)
					running = advance = true;
				if(e.type == SDL_KEYDOWN || e.type == SDL_MOUSEBUTTONDOWN)
					advance = true;
			}
			SDL_Delay(16);
		}
	}

	printf("agame: %d frame(s)\n", frame);
	fflush(stdout);
	SDL_DestroyTexture(screen.texture);
	SDL_DestroyRenderer(screen.renderer);
	SDL_DestroyWindow(screen.window);
	SDL_Quit();
	(void)outPath;
	return 0;
}

// ===========================================================================
// the real-script player
// ===========================================================================

namespace
{
	typedef std::chrono::steady_clock Clock;

	double MsSince(const Clock::time_point& from)
	{
		return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
	}

	// wall-clock time depends on what else the machine is doing; CPU time is the
	// number that belongs to this program
	double CpuMs()
	{
		struct rusage usage;
		if(getrusage(RUSAGE_SELF, &usage) != 0)
			return 0;
		return (double)usage.ru_utime.tv_sec * 1000.0 +
			(double)usage.ru_utime.tv_usec / 1000.0 +
			(double)usage.ru_stime.tv_sec * 1000.0 +
			(double)usage.ru_stime.tv_usec / 1000.0;
	}

	long long PeakRssBytes()
	{
		struct rusage usage;
		if(getrusage(RUSAGE_SELF, &usage) != 0)
			return 0;
		return (long long)usage.ru_maxrss; // bytes on macOS
	}

	struct Options
	{
		std::string script;
		std::string archive;
		std::string dataDir;
		std::string boot = "first.iet";
		std::string label = "start";
		std::string frames;
		std::string framePrefix = "page";
		std::string out = "screenshots/real-first-screen.png";
		int maxPages = 8;
		int captureTransitionMs = 0;
		int fadeFrames = 0;
		bool verbose = false;
		bool selftest = false;

		// the shipped-.ast path
		std::string astPath;      // the .ast to play, when one was named on the line
		std::string astInfoPath;  // --ast-info: parse and report only, never draw
		std::string size = "1600x900"; // the stage the corpus' own backgrounds imply
		std::string font;         // --font <path>: override the face lookup
		std::string startBlock;   // --start-block: jump into the chain
		bool showText = false;    // print the words (all-ages data only)
		int maxBlocks = 0;        // 0 = the whole linknext chain
		// the interactive window
		bool play = false;
		bool startAtMenu = true;    // boot -> menu -> click start -> the story
		bool realTime = false;      // wall clock + pacing (a real window)
		bool vsync = true;
		bool hashFrames = false;    // keep a hash per frame for a regression run
		bool outGiven = false;
		bool framesGiven = false;
		std::vector<std::string> autoplayKeys; // scripted input tokens
		std::string shotTitle;
		std::string shotScene;
		std::string shotDir;
		int quitAfterPages = 0;
		// audio and effects: opt-in, so the still-frame baseline stays exactly
		// what it was
		std::string audioCapture;   // --audio-capture <file.wav>
		double audioSeconds = 8.0;  // --audio-seconds N
		bool audio = false;         // --audio: wire the tags, no WAV
		bool fx = false;            // --fx: wire the effect tags
		double fxAt = -1;           // --fx-at: sample the effects at this offset
		int width = 1600;
		int height = 900;
		bool haveSize = false;
	};

	struct StageSettings
	{
		int width = 1024;
		int height = 698;
		int fps = 60;
		bool fromIni = false;
	};

	StageSettings ReadSystemIni(const std::string& text)
	{
		StageSettings out;
		size_t at = 0;
		while(at < text.size())
		{
			const size_t end = text.find('\n', at);
			std::string line = text.substr(at, end == std::string::npos ? end : end - at);
			at = end == std::string::npos ? text.size() : end + 1;
			while(!line.empty() && (line.back() == '\r' || line.back() == ' '))
				line.pop_back();
			const size_t equals = line.find('=');
			if(equals == std::string::npos)
				continue;
			std::string key = line.substr(0, equals);
			while(!key.empty() && (key.front() == ' ' || key.front() == '\t'))
				key.erase(key.begin());
			while(!key.empty() && (key.back() == ' ' || key.back() == '\t'))
				key.pop_back();
			for(char& c : key)
				c = (char)toupper((unsigned char)c);
			const int value = atoi(line.c_str() + equals + 1);
			if(key == "WIDTH" && value > 0)
			{
				out.width = value;
				out.fromIni = true;
			}
			else if(key == "HEIGHT" && value > 0)
			{
				out.height = value;
				out.fromIni = true;
			}
			else if(key == "FPS" && value > 0)
				out.fps = value;
		}
		return out;
	}

	size_t RegionColors(const artemis::Canvas& canvas, int x, int y, int w, int h)
	{
		std::unordered_set<uint32_t> seen;
		for(int yy = y; yy < y + h && yy < canvas.height(); yy++)
			for(int xx = x; xx < x + w && xx < canvas.width(); xx++)
				seen.insert(canvas.At(xx, yy) & 0xffffffu);
		return seen.size();
	}

	bool MakeParent(const std::string& path)
	{
		const size_t slash = path.rfind('/');
		if(slash == std::string::npos)
			return true;
		const std::string dir = path.substr(0, slash);
		if(dir.empty())
			return true;
		if(mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST)
			return true;
		return false;
	}

	// Everything one run measures, so the report and the self test read the same
	// numbers instead of each computing their own.
	struct ScreenRun
	{
		bool ok = false;
		std::string error;

		int width = 0;
		int height = 0;
		int fps = 0;
		bool sizeFromIni = false;

		int pages = 0;
		size_t textPixels = 0;  // framebuffer diff, with words minus without
		size_t inkPixels = 0;   // the same measured from the text bitmap's alpha
		size_t frameColors = 0;
		size_t backgroundColors = 0; // distinct colours in the top third
		int glyphs = 0;
		std::string speaker;
		std::string captureSpeaker;
		size_t captureTextPixels = 0;
		size_t captureInkPixels = 0;
		size_t captureColors = 0;
		size_t captureBackgroundColors = 0;
		int captureGlyphs = 0;
		int captureTransitionMs = 0;
		int fadeFrames = 0;
		int fadeMs = 0;
		int transitions = 0;

		bool captured = false;
		std::string capturePath;
		int pngWidth = 0;
		int pngHeight = 0;
		size_t pngColors = 0;
		size_t pngTextPixels = 0;
		bool readBack = false;

		// interpreter
		size_t commands = 0;
		size_t macroCalls = 0;
		size_t macroTable = 0;
		size_t macroDistinctUsed = 0;
		size_t luaTags = 0;
		size_t conditions = 0;
		size_t conditionErrors = 0;
		size_t waits = 0;
		long long waitMs = 0;
		size_t unknown = 0;
		size_t ignored = 0;
		size_t inlineTags = 0;
		size_t assetFailures = 0;
		std::string bootSummary;
		std::string assetFailureText;
		// audio and effects
		size_t audioTracks = 0;
		bool audioCaptured = false;
		std::string audioPath;
		double audioSeconds = 0;
		double audioRms = 0;
		double audioPeak = 0;
		bool audioSilent = true;
		long long audioFrames = 0;
		std::vector<std::string> audioLines;
		std::vector<std::string> audioNotes;
		size_t fxTags = 0;
		std::vector<std::string> fxNotes;
		// input wiring and choices
		std::vector<std::string> bindings;   // setonpush, declared in the loaded scripts
		std::vector<std::string> choices;    // select tags
		size_t bindingsExecuted = 0;
		std::map<std::string, long long> frontEndTags;
		std::vector<std::pair<std::string, long long>> unknownTags;
		std::vector<std::pair<std::string, long long>> ignoredTags;
		std::vector<std::pair<std::string, std::string>> unknownWhere;
		std::vector<std::string> loadedScripts;
		std::vector<std::string> scriptLines;
		std::vector<std::string> styles;
		std::vector<std::string> layerIds;
		std::string layerDetail;
		size_t layerCount = 0;
		std::vector<std::string> fontFaces;
		std::vector<std::string> fontEvidence;

		// perf
		double bootMs = 0;
		double fontMs = 0;
		double firstCompositeMs = 0; // process start -> the captured frame composited
		double firstFrameMs = 0;     // process start -> that frame written and verified
		double perFrameMs = 0;
		double compositeMs = 0;
		double censusMs = 0;
		double pngMs = 0;
		double captureCompositeMs = 0;
		double captureCompositeCpuMs = 0;
		double cpuTotalMs = 0;
		double captureExtraMs = 0;
		double captureWriteMs = 0;
		double captureReadBackMs = 0;
		double firstPageCompositeMs = 0;
		double loadMs = 0;
		size_t textureHits = 0;
		size_t textureMisses = 0;
		size_t textureBytes = 0;
		double textureReadMs = 0;
		double textureDecodeMs = 0;
		long long peakRss = 0;
	};

	ScreenRun Play(const Options& options, bool quiet);
}

// ===========================================================================
// the audio layer, and the WAV it leaves behind
// ===========================================================================
//
// A script asks for sound the way it asks for a picture: `[bgm file="moon"]`,
// `[seplay id="3" file="forsystem/se/cursor1"]`, and in the .ast corpus
// `{"bgm", file="bgm02"}` / `{"se", id=1, file="balican"}`.  The tags reach this
// file two ways - through Vm::TagSink for .iet, through AstRun's SceneEvents for
// .ast - and both land on one AudioBridge.
//
// The awkward part is that the engine wants a *directory* of .ogg files while
// the game keeps them inside an archive.  So an asset is copied out of the
// archive into a work directory the first time a tag names it, and the engine's
// own name is the sanitised member path.  Nothing else about the layer is
// re-implemented here: decoding, rate conversion, the .sli loop window and the
// mixer are all `artemis::audio`.
//
// `--audio-capture out.wav` mixes offline: the engine's decoder thread fills its
// rings, this loop stands in for the device clock (wait for the buffer, then
// Mix), and the result is written as 16-bit PCM.  That is what makes a headless
// run able to prove sound: a WAV with a length, a peak, an RMS, and per-track
// numbers measured by decoding each track on its own.

#include "../src/audio/audio.h"

namespace
{
	struct AudioTrackReport
	{
		std::string kind;     // bgm / se / voice
		std::string asked;    // the name in the script
		std::string resolved; // the archive member that answered
		std::string engineName;
		double seconds = 0;
		double rms = 0;
		double peak = 0;
		bool silent = true;
		bool loop = false;
		bool started = false;
		std::string note;
	};

	class AudioBridge
	{
	public:
		struct Config
		{
			const artemis::AssetSource* assets = nullptr;
			std::string workDir;
			int sampleRate = 44100;
			double measureSeconds = 5.0; // how much of each track to solo-measure
		};

		~AudioBridge() { Close(); }

		bool Open(const Config& config, std::string& error);
		void Close();
		bool open() const { return m_opened; }

		// one tag, already expanded by the interpreter; false when it could not be
		// acted on (and `note` says why)
		bool OnTag(const std::string& name, const std::map<std::string, std::string>& attrs,
			std::string& note);
		bool OnSceneEvent(const artemis::SceneEvent& event, std::string& note);

		bool Capture(const std::string& path, double seconds, std::string& error);
		// the device callback's entry point: the engine's Mix, no locks, no I/O
		void Mix(float* out, int frames)
		{
			if(m_opened)
				m_engine.Mix(out, frames);
		}

		const std::vector<AudioTrackReport>& tracks() const { return m_tracks; }
		const std::vector<std::string>& notes() const { return m_notes; }
		bool captured() const { return m_captured; }
		double capturedSeconds() const { return m_capturedSeconds; }
		double capturedRms() const { return m_capturedRms; }
		double capturedPeak() const { return m_capturedPeak; }
		bool capturedSilent() const { return m_capturedSilent; }
		int64_t capturedFrames() const { return m_capturedFrames; }
		const std::string& capturedPath() const { return m_capturedPath; }

	private:
		bool Materialize(const std::string& base, const std::string& kind, bool loop,
			std::string& engineName, std::string& resolved, std::string& note);
		bool Play(const std::string& name, const std::map<std::string, std::string>& attrs,
			const std::string& kind, std::string& note);
		void Measure(AudioTrackReport& track);

		Config m_config;
		artemis::audio::Engine m_engine;
		bool m_opened = false;
		std::vector<AudioTrackReport> m_tracks;
		std::vector<std::string> m_notes;
		std::map<std::string, size_t> m_byName; // engine name -> track index
		bool m_captured = false;
		std::string m_capturedPath;
		double m_capturedSeconds = 0;
		double m_capturedRms = 0;
		double m_capturedPeak = 0;
		bool m_capturedSilent = true;
		int64_t m_capturedFrames = 0;
	};

	bool AudioBridge::Open(const Config& config, std::string& error)
	{
		m_config = config;
		if(config.assets == nullptr)
		{
			error = "no assets";
			return false;
		}
		if(mkdir(m_config.workDir.c_str(), 0755) != 0 && errno != EEXIST)
		{
			error = "cannot create the audio work directory " + m_config.workDir;
			return false;
		}
		artemis::audio::Engine::Config engine;
		engine.assetRoot = m_config.workDir;
		engine.sampleRate = m_config.sampleRate;
		std::string engineError;
		if(!m_engine.Open(engine, &engineError))
		{
			error = "audio engine: " + engineError;
			return false;
		}
		m_opened = true;
		return true;
	}

	void AudioBridge::Close()
	{
		if(m_opened)
			m_engine.Shutdown();
		m_opened = false;
	}

	// Copy one archive member out to the work directory.  `assets` resolves a
	// logical name to a member; the extension is explicit because the ogg names
	// are not in AssetSource's image-extension list.
	bool AudioBridge::Materialize(const std::string& base, const std::string& kind, bool loop,
		std::string& engineName, std::string& resolved, std::string& note)
	{
		std::vector<std::string> candidates;
		std::string cleaned = base;
		for(char& c : cleaned)
			if(c == '\\')
				c = '/';
		if(cleaned.size() > 4 && cleaned.compare(cleaned.size() - 4, 4, ".ogg") == 0)
			candidates.push_back(cleaned);
		else
			candidates.push_back(cleaned + ".ogg");
		// the script may name a bare "moon" or a path ("bgm/moon"): try the roots a
		// shipped Artemis title uses
		static const char* roots[] = { "", "bgm/", "sound/bgm/", "se/", "sound/se/",
			"sound/vo/", "voice/", "sound/bgv/" };
		for(const char* root : roots)
			if(!cleaned.empty() && cleaned[0] != '/' &&
				cleaned.compare(0, std::strlen(root), root) != 0)
				candidates.push_back(std::string(root) + cleaned + ".ogg");
		for(const std::string& candidate : candidates)
		{
			std::vector<uint8_t> bytes;
			std::string readResolved, readError;
			if(!m_config.assets->Read(candidate, bytes, readResolved, readError))
				continue;
			// the engine's name is built from the *logical* name, not from the path
			// the member resolved to: an absolute path would hand the engine a name
			// with ".." in it, which it (rightly) refuses
			std::string stem = candidate;
			if(stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".png") == 0)
				continue; // an image answered: this name is not sound
			for(char& c : stem)
				if(c == '/' || c == '\\' || c == ':' || c == ' ')
					c = '_';
			while(!stem.empty() && stem[0] == '.')
				stem.erase(stem.begin());
			const std::string path = m_config.workDir + "/" + stem;
			FILE* file = fopen(path.c_str(), "wb");
			if(file == nullptr)
			{
				note = "cannot write " + path;
				return false;
			}
			fwrite(bytes.data(), 1, bytes.size(), file);
			fclose(file);
			// the .sli, when the release ships one, is a *separate member*
			std::vector<uint8_t> sli;
			std::string sliResolved, sliError;
			if(m_config.assets->Read(candidate + ".sli", sli, sliResolved, sliError))
			{
				FILE* sliFile = fopen((path + ".sli").c_str(), "wb");
				if(sliFile != nullptr)
				{
					fwrite(sli.data(), 1, sli.size(), sliFile);
					fclose(sliFile);
				}
			}
			engineName = stem;
			if(engineName.size() > 4)
				engineName = engineName.substr(0, engineName.size() - 4);
			resolved = readResolved;
			(void)kind;
			(void)loop;
			return true;
		}
		note = "no ogg member answers to \"" + base + "\"";
		return false;
	}

	void AudioBridge::Measure(AudioTrackReport& track)
	{
		if(track.engineName.empty())
			return;
		const std::string path = m_config.workDir + "/" + track.engineName + ".ogg";
		std::string error;
		std::unique_ptr<artemis::audio::Source> source = artemis::audio::Source::Open(path,
			&error);
		if(source == nullptr)
		{
			track.note = "cannot decode: " + error;
			return;
		}
		track.seconds = source->Info().Seconds();
		artemis::audio::Stream::Config streamConfig;
		streamConfig.engineRate = m_config.sampleRate;
		std::shared_ptr<artemis::audio::Stream> stream = artemis::audio::Stream::Open(path,
			streamConfig, &error);
		if(stream == nullptr)
		{
			track.note = "cannot stream: " + error;
			return;
		}
		stream->Start(artemis::audio::LoopPlan::Whole(source->Info().frames));
		const int64_t want = (int64_t)(std::min(track.seconds,
			m_config.measureSeconds <= 0 ? track.seconds : m_config.measureSeconds) *
			m_config.sampleRate);
		const int tick = 512;
		std::vector<float> block((size_t)tick * 2, 0.0f);
		double sum = 0;
		int64_t frames = 0;
		while(frames < want)
		{
			const int n = (int)std::min<int64_t>(tick, want - frames);
			for(int tries = 0; tries < 256 && stream->Buffered() < n; tries++)
				if(stream->Fill() == 0 && (stream->Finished() || stream->Stopped()))
					break;
			std::fill(block.begin(), block.end(), 0.0f);
			stream->Mix(block.data(), n);
			for(int i = 0; i < n * 2; i++)
			{
				const double sample = (double)block[(size_t)i];
				sum += sample * sample;
				const double magnitude = std::fabs(sample);
				if(magnitude > track.peak)
					track.peak = magnitude;
			}
			frames += n;
		}
		if(frames > 0)
			track.rms = std::sqrt(sum / (double)(frames * 2));
		track.silent = track.peak < (double)artemis::audio::kSilenceFloor;
	}

	bool AudioBridge::Play(const std::string& name,
		const std::map<std::string, std::string>& attrs, const std::string& kind,
		std::string& note)
	{
		if(!m_opened)
		{
			note = "the audio engine is not open";
			return false;
		}
		// `$'bgm/' + file`: a Lua expression the tag sink cannot evaluate.  Saying
		// so is the whole point - silently playing nothing would look like success.
		if(!name.empty() && (name[0] == '$' || name[0] == '%' || name.find("..") !=
			std::string::npos || name.find(" + ") != std::string::npos))
		{
			note = "the script passes an expression (\"" + name +
				"\"), not a name: not evaluated here";
			m_notes.push_back(note);
			return false;
		}
		const bool loop = attrs.count("loop") != 0 && attrs.at("loop") != "0";
		const int fadeMs = attrs.count("time") != 0 ? atoi(attrs.at("time").c_str()) : 0;
		std::string engineName, resolved;
		if(!Materialize(name, kind, loop, engineName, resolved, note))
		{
			m_notes.push_back(note);
			return false;
		}
		AudioTrackReport track;
		track.kind = kind;
		track.asked = name;
		track.resolved = resolved;
		track.engineName = engineName;
		track.loop = loop;
		std::map<std::string, size_t>::const_iterator known = m_byName.find(engineName);
		if(known != m_byName.end())
			track = m_tracks[known->second];
		else
			Measure(track);
		if(kind == "bgm" || kind == "splay")
			track.started = m_engine.PlayBgm(engineName, loop, fadeMs);
		else
			track.started = m_engine.PlaySe(engineName, -1);
		if(!track.started)
		{
			note = "the engine refused " + engineName;
			m_notes.push_back(note);
			return false;
		}
		if(known == m_byName.end())
		{
			m_byName[engineName] = m_tracks.size();
			m_tracks.push_back(track);
		}
		else
			m_tracks[known->second].started = true;
		return true;
	}

	bool AudioBridge::OnTag(const std::string& name, const std::map<std::string, std::string>& attrs,
		std::string& note)
	{
		std::string file = attrs.count("file") != 0 ? attrs.at("file") : std::string();
		// [mbgm file="bgm01,bgm11,bgm21" default="bgm01"]: a set to pick from, not a
		// name.  The `default` is what a run with no choice would start.
		if(file.find(',') != std::string::npos)
		{
			const std::string chosen = attrs.count("default") != 0
				? attrs.at("default")
				: file.substr(0, file.find(','));
			note = "mbgm offers \"" + file + "\": using \"" + chosen + "\"";
			m_notes.push_back(note);
			file = chosen;
			note.clear();
		}
		if(name == "bgm" || name == "splay" || name == "sxfade" || name == "mbgm")
			return Play(file, attrs, name == "seplay" ? "se" : "bgm", note);
		if(name == "seplay" || name == "se" || name == "playse")
			return Play(file, attrs, "se", note);
		if(name == "voice" || name == "vo")
			return Play(file, attrs, "voice", note);
		if(name == "bgmstop" || name == "sstop" || name == "vstop" || name == "stopse" ||
			name == "sestop")
		{
			if(m_opened)
				m_engine.StopBgm(0);
			m_notes.push_back("stopped by [" + name + "]");
			return true;
		}
		note.clear(); // not an audio tag at all: not a failure, just not ours
		return false;
	}

	bool AudioBridge::OnSceneEvent(const artemis::SceneEvent& event, std::string& note)
	{
		if(event.kind != artemis::SceneEvent::Kind::Audio)
			return false;
		const std::string& name = event.command;
		if(event.file.empty() && name != "bgtstop" && name != "mbgmstop")
		{
			// `{"se", id=1}` with no file is a stop for that channel, not a lookup
			note.clear();
			return true;
		}
		if(name == "bgm" || name == "chmbgm" || name == "mbgm")
			return Play(event.file, event.attrs, "bgm", note);
		if(name == "se")
			return Play(event.file, event.attrs, "se", note);
		if(name == "vo" || name == "lvo")
			return Play(event.file, event.attrs, "voice", note);
		if(name == "bgtstop" || name == "mbgmstop")
		{
			if(m_opened)
				m_engine.StopBgm(0);
			m_notes.push_back("stopped by {" + name + "}");
			return true;
		}
		note = "audio command not modelled: " + name;
		return false;
	}

	bool WriteWav16(const std::string& path, const std::vector<float>& interleaved, int rate,
		std::string& error)
	{
		const uint32_t frames = (uint32_t)(interleaved.size() / 2);
		const uint32_t dataBytes = frames * 4; // 2 channels, 16 bit
		FILE* file = fopen(path.c_str(), "wb");
		if(file == nullptr)
		{
			error = "cannot write " + path;
			return false;
		}
		auto put32 = [&](uint32_t value) {
			const uint8_t bytes[4] = { (uint8_t)(value & 0xff), (uint8_t)((value >> 8) & 0xff),
				(uint8_t)((value >> 16) & 0xff), (uint8_t)((value >> 24) & 0xff) };
			fwrite(bytes, 1, 4, file);
		};
		auto put16 = [&](uint16_t value) {
			const uint8_t bytes[2] = { (uint8_t)(value & 0xff), (uint8_t)((value >> 8) & 0xff) };
			fwrite(bytes, 1, 2, file);
		};
		fwrite("RIFF", 1, 4, file);
		put32(36 + dataBytes);
		fwrite("WAVE", 1, 4, file);
		fwrite("fmt ", 1, 4, file);
		put32(16);
		put16(1);       // PCM
		put16(2);       // channels
		put32((uint32_t)rate);
		put32((uint32_t)rate * 4);
		put16(4);       // block align
		put16(16);      // bits
		fwrite("data", 1, 4, file);
		put32(dataBytes);
		for(float sample : interleaved)
		{
			double value = (double)sample * 32767.0;
			if(value > 32767.0)
				value = 32767.0;
			if(value < -32768.0)
				value = -32768.0;
			put16((uint16_t)(int16_t)std::lround(value));
		}
		fclose(file);
		return true;
	}

	bool AudioBridge::Capture(const std::string& path, double seconds, std::string& error)
	{
		if(!m_opened)
		{
			error = "the audio engine is not open";
			return false;
		}
		const int rate = m_config.sampleRate;
		const int64_t total = (int64_t)(seconds * rate);
		const int tick = 1024;
		std::vector<float> block((size_t)tick * 2, 0.0f);
		std::vector<float> out;
		out.reserve((size_t)total * 2);
		double sum = 0;
		double peak = 0;
		int64_t done = 0;
		while(done < total)
		{
			const int n = (int)std::min<int64_t>(tick, total - done);
			// stand in for the device clock: a headless loop would drain the mixer
			// faster than the decoder thread fills it and report silence
			for(int tries = 0; tries < 500; tries++)
			{
				if(m_engine.BufferedFrames() >= n)
					break;
				if(tries > 40 && m_engine.ActiveStreams() == 0)
					break; // everything has finished: the rest is silence
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			std::fill(block.begin(), block.end(), 0.0f);
			m_engine.Mix(block.data(), n);
			for(int i = 0; i < n * 2; i++)
			{
				const double sample = (double)block[(size_t)i];
				sum += sample * sample;
				const double magnitude = std::fabs(sample);
				if(magnitude > peak)
					peak = magnitude;
				out.push_back(block[(size_t)i]);
			}
			done += n;
		}
		m_capturedFrames = done;
		m_capturedSeconds = (double)done / (double)rate;
		m_capturedRms = done > 0 ? std::sqrt(sum / (double)(done * 2)) : 0;
		m_capturedPeak = peak;
		m_capturedSilent = peak < (double)artemis::audio::kSilenceFloor;
		const size_t slash = path.rfind('/');
		if(slash != std::string::npos)
			mkdir(path.substr(0, slash).c_str(), 0755);
		if(!WriteWav16(path, out, rate, error))
			return false;
		m_captured = true;
		m_capturedPath = path;
		return true;
	}
}

// ===========================================================================
// the effects layer, driven by a clock this program owns
// ===========================================================================
//
// Every effect in this section is a *function of time*, not a step in a loop:
// given t it answers "what is the value now".  That is what makes a still-frame
// renderer able to prove an animation exists - sample it at two times and the
// two answers must differ in the way the script asked for - and it is also why
// nothing here sleeps.  `--fx-check` walks the six effects the corpus actually
// uses, at times taken from the real tags, and asserts on the numbers and on
// the pixels.
//
//   [scetween]   relative: value = base + diff * curve(t).  base is the value
//                *when the tag arrived*, which is why two scetweens in a row
//                accumulate instead of both starting from the same number.
//   [lytween]    absolute: from -> to, `yoyo="-1"` runs it back and forth
//                forever.  A tag with no `to` is refused by fx::ParseLytween and
//                counted here; real data contains bad data.
//   [anime]      a keyframe track: each key is a *jump* at its `time`, with no
//                interpolation between keys.
//   [sfade]      a brightness target plus a duration; gain 1000 is the identity.
//   [quake]      a seeded offset: same seed, same shake, every run.
//   g.msgspeed   not a tag: first.iet sets it to 30, and the message layer
//                reveals one glyph every 30 ms.  fx::typewriter does the glyph
//                boundaries; this file does the clock.

#include "../src/fx/animation.h"
#include "../src/fx/brightness.h"
#include "../src/fx/quake.h"
#include "../src/fx/tween.h"
#include "../src/fx/typewriter.h"

namespace
{
	struct FxCounters
	{
		long long msgspeedSet = 0;
		long long scetween = 0;
		long long scetweenInit = 0;
		long long scetweenRefused = 0;
		long long lytween = 0;
		long long lytweenRefused = 0;
		long long lytweenDeleted = 0;
		long long anime = 0;
		long long animeTracks = 0;
		long long sfade = 0;
		long long quake = 0;
		long long flash = 0;
		long long fades = 0;
		long long unmatched = 0;
		std::vector<std::string> notes;
	};

	struct LayerAnim
	{
		enum class Kind
		{
			Scetween,
			Lytween,
			Anime,
		};

		Kind kind = Kind::Scetween;
		std::string id;
		std::string param;
		int64_t startMs = 0;
		int64_t base = 0;
		artemis::fx::Scetween scetween;
		artemis::fx::Tween tween;
		artemis::fx::AnimeTrack anime;
		artemis::fx::AnimeSample lastSample;
	};

	class FxDriver
	{
	public:
		// --- text -----------------------------------------------------------
		void SetMsgspeed(int msPerGlyph)
		{
			m_msgspeed = msPerGlyph;
			m_counters.msgspeedSet++;
		}
		int msgspeed() const { return m_msgspeed; }
		void SetTypewriter(bool on) { m_typewriter = on; }
		bool typewriter() const { return m_typewriter; }

		void BeginPage(const std::string& text, int64_t nowMs)
		{
			m_page = artemis::fx::Segment(text);
			m_pageStartMs = nowMs;
			m_counters.notes.push_back("page: " + std::to_string(m_page.timedGlyphs) +
				" timed glyph(s), " + std::to_string(m_page.stops.size()) + " stop(s)");
		}
		artemis::fx::Reveal Reveal(int64_t nowMs) const
		{
			return artemis::fx::RevealAt(m_page, nowMs - m_pageStartMs, m_msgspeed);
		}
		bool pageComplete(int64_t nowMs) const { return Reveal(nowMs).complete; }
		int64_t pageDurationMs() const
		{
			return artemis::fx::RevealDurationMs(m_page, m_msgspeed);
		}
		size_t pageSize() const { return m_page.utf8.size(); }

		// --- tags -----------------------------------------------------------
		void OnTag(const artemis::Vm::TagEvent& tag, int64_t nowMs, artemis::Stage* stage);
		void OnSceneEvent(const artemis::SceneEvent& event, int64_t nowMs);
		void OnScetween(const std::map<std::string, std::string>& attrs, int64_t nowMs,
			artemis::Stage* stage);
		void OnLytween(const std::map<std::string, std::string>& attrs, int64_t nowMs);
		void OnLytweenDelete(const std::map<std::string, std::string>& attrs);
		void OnAnime(const std::map<std::string, std::string>& attrs, int64_t nowMs);
		void OnSfade(const std::map<std::string, std::string>& attrs, int64_t nowMs);
		void OnQuake(const std::map<std::string, std::string>& attrs, int64_t nowMs);
		void OnFlash(const std::map<std::string, std::string>& attrs, int64_t nowMs);

		// --- per frame ------------------------------------------------------
		void ApplyLayers(artemis::Stage& stage, int64_t nowMs);
		int64_t LayerValue(const std::string& id, const std::string& param, int64_t nowMs) const;
		int Gain(int64_t nowMs) const;
		void ApplyCanvas(artemis::Canvas& canvas, int64_t nowMs) const;
		int Dx(int64_t nowMs) const { return QuakeOffset(nowMs).dx; }
		int Dy(int64_t nowMs) const { return QuakeOffset(nowMs).dy; }
		bool Active(int64_t nowMs) const;

		const FxCounters& counters() const { return m_counters; }
		FxCounters& counters() { return m_counters; }
		const std::vector<LayerAnim>& anims() const { return m_anims; }

	private:
		artemis::fx::QuakeOffset QuakeOffset(int64_t nowMs) const;
		LayerAnim* FindAnim(const std::string& id, const std::string& param);

		artemis::fx::Text m_page;
		int64_t m_pageStartMs = 0;
		int m_msgspeed = 30;
		bool m_typewriter = false;
		FxCounters m_counters;
		std::vector<LayerAnim> m_anims;

		struct Fade
		{
			int from = 1000;
			int to = 1000;
			int64_t startMs = 0;
			int64_t timeMs = 0;
			bool active = false;
		};
		Fade m_fade;

		struct Flash
		{
			uint8_t color[3] = { 255, 255, 255 };
			int alpha = 0;
			int64_t startMs = 0;
			int64_t timeMs = 0;
			int loops = 1;
			bool active = false;
		};
		Flash m_flash;

		struct Shake
		{
			artemis::fx::Quake quake;
			int64_t startMs = 0;
			bool active = false;
		};
		Shake m_shake;
	};

	std::string AttrOf(const std::map<std::string, std::string>& attrs, const std::string& name,
		const std::string& fallback = std::string())
	{
		std::map<std::string, std::string>::const_iterator found = attrs.find(name);
		return found == attrs.end() ? fallback : found->second;
	}

	// A VK code as the key it is.  The scripts bind raw Windows virtual-key
	// numbers (`key="83"`), and a table of numbers is not a binding table; this is
	// the mapping the numbers already have.
	std::string VirtualKeyName(int code)
	{
		if(code >= 'A' && code <= 'Z')
			return std::string(1, (char)code);
		if(code >= '0' && code <= '9')
			return std::string(1, (char)code);
		switch(code)
		{
		case 8: return "BACK";
		case 9: return "TAB";
		case 13: return "RETURN";
		case 27: return "ESCAPE";
		case 32: return "SPACE";
		case 37: return "LEFT";
		case 38: return "UP";
		case 39: return "RIGHT";
		case 40: return "DOWN";
		case 46: return "DELETE";
		case 96: return "NUM0";
		case 97: return "NUM1";
		case 98: return "NUM2";
		case 99: return "NUM3";
		case 100: return "NUM4";
		case 101: return "NUM5";
		case 102: return "NUM6";
		case 103: return "NUM7";
		case 104: return "NUM8";
		case 105: return "NUM9";
		case 106: return "NUM*";
		case 107: return "NUM+";
		case 109: return "NUM-";
		case 110: return "NUM.";
		case 111: return "NUM/";
		case 112: return "F1";
		case 113: return "F2";
		case 114: return "F3";
		case 115: return "F4";
		case 116: return "F5";
		case 117: return "F6";
		case 118: return "F7";
		case 119: return "F8";
		case 120: return "F9";
		case 121: return "F10";
		case 122: return "F11";
		case 123: return "F12";
		default: break;
		}
		return "VK" + std::to_string(code);
	}

	LayerAnim* FxDriver::FindAnim(const std::string& id, const std::string& param)
	{
		for(LayerAnim& anim : m_anims)
			if(anim.id == id && anim.param == param)
				return &anim;
		return nullptr;
	}

	void FxDriver::OnTag(const artemis::Vm::TagEvent& tag, int64_t nowMs,
		artemis::Stage* stage)
	{
		if(tag.name == "scetween")
			OnScetween(tag.attributes, nowMs, stage);
		else if(tag.name == "lytween")
			OnLytween(tag.attributes, nowMs);
		else if(tag.name == "lytweendel")
			OnLytweenDelete(tag.attributes);
		else if(tag.name == "anime")
			OnAnime(tag.attributes, nowMs);
		else if(tag.name == "sfade" || tag.name == "fade" || tag.name == "fadein" ||
			tag.name == "fadeout")
			OnSfade(tag.attributes, nowMs);
		else if(tag.name == "quake")
			OnQuake(tag.attributes, nowMs);
		else if(tag.name == "flash")
			OnFlash(tag.attributes, nowMs);
	}

	// a `.ast` command, in the same vocabulary.  The corpus has four of these:
	// quake (61), flash (11), extrans (34) and colortone (141).  Two of them are
	// modelled; the two that need a second picture or a colour matrix are counted
	// and named here instead of being applied as something they are not.
	void FxDriver::OnSceneEvent(const artemis::SceneEvent& event, int64_t nowMs)
	{
		if(event.command == "quake")
			OnQuake(event.attrs, nowMs);
		else if(event.command == "flash")
			OnFlash(event.attrs, nowMs);
		else if(event.command == "extrans")
		{
			m_counters.fades++;
			if(m_counters.notes.size() < 64)
				m_counters.notes.push_back("extrans rule=" + event.Str("rule", "-") +
					" time=" + std::to_string(event.Int("time", 0)) +
					": a crossfade needs two pictures, not modelled");
		}
		else if(event.command == "colortone")
		{
			if(m_counters.notes.size() < 64)
				m_counters.notes.push_back("colortone style=\"" + event.Str("style", "-") +
					"\": a colour matrix is not modelled");
		}
	}

	void FxDriver::OnScetween(const std::map<std::string, std::string>& attrs, int64_t nowMs,
		artemis::Stage* stage)
	{
		const std::string mode = AttrOf(attrs, "mode", "add");
		const std::string slot = AttrOf(attrs, "type", AttrOf(attrs, "id"));
		const std::string param = AttrOf(attrs, "param", "alpha");
		if(slot.empty())
		{
			m_counters.scetweenRefused++;
			m_counters.notes.push_back("scetween without a type/id: ignored");
			return;
		}
		// `type` is a GUI state slot.  Until the title's own GUI script is read, a
		// slot is modelled as the layer of the same name: a scetween for a slot with
		// no layer is counted as unmatched, not applied to some other layer.
		if(stage != nullptr && !stage->HasLayer(slot))
		{
			m_counters.unmatched++;
			if(m_counters.notes.size() < 64)
				m_counters.notes.push_back("scetween slot \"" + slot + "\" has no layer");
		}
		if(mode == "init")
		{
			// the init tags carry no value: they set the slot's resting state, which
			// for the message window is "in"/"show" = visible, "out"/"hide" = gone
			m_counters.scetweenInit++;
			const bool shown = slot == "in" || slot == "show";
			const int64_t value = shown ? 255 : 0;
			m_anims.erase(std::remove_if(m_anims.begin(), m_anims.end(),
				[&](const LayerAnim& a) { return a.id == slot && a.param == param; }),
				m_anims.end());
			LayerAnim set;
			set.kind = LayerAnim::Kind::Scetween;
			set.id = slot;
			set.param = param;
			set.startMs = nowMs;
			set.base = value;
			set.scetween.diff = 0;
			set.scetween.durationMs = 0;
			set.scetween.delayMs = 0;
			m_anims.push_back(set);
			return;
		}

		LayerAnim anim;
		anim.kind = LayerAnim::Kind::Scetween;
		anim.id = slot;
		anim.param = param;
		anim.startMs = nowMs;
		// the base is the value the slot has *now*: the layer's own value when the
		// slot is a layer, otherwise whatever the previous scetween on that slot
		// reached.  That is the whole of "the diff is an increment".
		anim.base = LayerValue(slot, param, nowMs);
		if(stage != nullptr)
		{
			int onStage = 0;
			if(stage->LayerProperty(slot, param, onStage))
				anim.base = onStage;
		}
		artemis::fx::Ease ease = artemis::fx::Ease::Linear;
		if(!artemis::fx::ParseEase(AttrOf(attrs, "ease", "none"), ease))
		{
			m_counters.notes.push_back("scetween ease \"" + AttrOf(attrs, "ease") +
				"\" is not a curve this port knows; linear");
			ease = artemis::fx::Ease::Linear;
		}
		anim.scetween.diff = atoll(AttrOf(attrs, "diff", "0").c_str());
		anim.scetween.durationMs = atoll(AttrOf(attrs, "time", "400").c_str());
		anim.scetween.delayMs = atoll(AttrOf(attrs, "delay", "0").c_str());
		anim.scetween.ease = ease;
		m_counters.scetween++;
		m_anims.push_back(anim);
	}

	void FxDriver::OnLytween(const std::map<std::string, std::string>& attrs, int64_t nowMs)
	{
		std::vector<artemis::fx::AttrNameValue> list;
		for(const std::pair<const std::string, std::string>& item : attrs)
		{
			artemis::fx::AttrNameValue pair;
			pair.name = item.first;
			pair.value = item.second;
			list.push_back(pair);
		}
		artemis::fx::LytweenTag tag;
		if(!artemis::fx::ParseLytween(list, tag) || !tag.usable)
		{
			// the corpus has two lytweens that type `top=` where `to=` belongs; the
			// point of refusing them is that guessing would animate a value no script
			// asked for
			m_counters.lytweenRefused++;
			m_counters.notes.push_back("lytween refused: " +
				(tag.note.empty() ? std::string("unreadable tag") : tag.note));
			return;
		}
		m_anims.erase(std::remove_if(m_anims.begin(), m_anims.end(),
			[&](const LayerAnim& a) {
				return a.kind == LayerAnim::Kind::Lytween && a.id == tag.id &&
					a.param == tag.param;
			}),
			m_anims.end());
		LayerAnim anim;
		anim.kind = LayerAnim::Kind::Lytween;
		anim.id = tag.id;
		anim.param = tag.param;
		anim.startMs = nowMs;
		anim.tween = tag.tween;
		m_counters.lytween++;
		m_anims.push_back(anim);
	}

	void FxDriver::OnLytweenDelete(const std::map<std::string, std::string>& attrs)
	{
		const std::string id = AttrOf(attrs, "id");
		m_anims.erase(std::remove_if(m_anims.begin(), m_anims.end(),
			[&](const LayerAnim& a) {
				return a.kind == LayerAnim::Kind::Lytween && a.id == id;
			}),
			m_anims.end());
		m_counters.lytweenDeleted++;
	}

	void FxDriver::OnAnime(const std::map<std::string, std::string>& attrs, int64_t nowMs)
	{
		const std::string id = AttrOf(attrs, "id");
		const std::string mode = AttrOf(attrs, "mode", "init");
		if(id.empty())
		{
			m_counters.notes.push_back("anime without an id: ignored");
			return;
		}
		LayerAnim* anim = nullptr;
		for(LayerAnim& candidate : m_anims)
			if(candidate.kind == LayerAnim::Kind::Anime && candidate.id == id)
				anim = &candidate;
		if(mode == "init" || anim == nullptr)
		{
			if(anim == nullptr)
			{
				LayerAnim fresh;
				fresh.kind = LayerAnim::Kind::Anime;
				fresh.id = id;
				fresh.param = "track";
				fresh.startMs = nowMs;
				m_anims.push_back(fresh);
				anim = &m_anims.back();
				m_counters.animeTracks++;
			}
			else
			{
				anim->anime = artemis::fx::AnimeTrack();
				anim->startMs = nowMs;
			}
			artemis::fx::AnimeKey key;
			key.file = AttrOf(attrs, "file");
			key.hasFile = !key.file.empty();
			key.top = atoi(AttrOf(attrs, "top", "0").c_str());
			key.left = atoi(AttrOf(attrs, "left", "0").c_str());
			key.alpha = atoi(AttrOf(attrs, "alpha", "255").c_str());
			key.hasTop = attrs.find("top") != attrs.end();
			key.hasLeft = attrs.find("left") != attrs.end();
			key.hasAlpha = attrs.find("alpha") != attrs.end();
			key.arriveMs = atoll(AttrOf(attrs, "time", "0").c_str());
			anim->anime.keys.push_back(key);
			m_counters.anime++;
			return;
		}
		if(mode == "end")
		{
			anim->anime.endHoldMs = atoll(AttrOf(attrs, "time", "0").c_str());
			m_counters.anime++;
			return;
		}
		// mode="add": the key arrives `time` after the previous one
		artemis::fx::AnimeKey key;
		key.file = AttrOf(attrs, "file");
		key.hasFile = !key.file.empty();
		key.top = atoi(AttrOf(attrs, "top", "0").c_str());
		key.left = atoi(AttrOf(attrs, "left", "0").c_str());
		key.alpha = atoi(AttrOf(attrs, "alpha", "255").c_str());
		key.hasTop = attrs.find("top") != attrs.end();
		key.hasLeft = attrs.find("left") != attrs.end();
		key.hasAlpha = attrs.find("alpha") != attrs.end();
		key.arriveMs = atoll(AttrOf(attrs, "time", "0").c_str());
		anim->anime.keys.push_back(key);
		m_counters.anime++;
	}

	void FxDriver::OnSfade(const std::map<std::string, std::string>& attrs, int64_t nowMs)
	{
		const int gain = artemis::fx::ClampGain(atoi(AttrOf(attrs, "gain", "1000").c_str()));
		m_fade.from = Gain(nowMs);
		m_fade.to = gain;
		m_fade.startMs = nowMs;
		m_fade.timeMs = atoll(AttrOf(attrs, "time", "0").c_str());
		m_fade.active = true;
		m_counters.sfade++;
	}

	void FxDriver::OnQuake(const std::map<std::string, std::string>& attrs, int64_t nowMs)
	{
		m_shake.quake.sizePx = atoi(AttrOf(attrs, "size", "0").c_str());
		m_shake.quake.count = std::max(1, atoi(AttrOf(attrs, "count", "1").c_str()));
		m_shake.quake.vertical = AttrOf(attrs, "dir", "h") == "v";
		m_shake.quake.timeMs = atoll(AttrOf(attrs, "time", "0").c_str());
		// the same seed every run: a shake that reseeded itself would make every
		// frame of a comparison move, which is not reproducible and not testable
		m_shake.quake.seed = (uint64_t)atoll(AttrOf(attrs, "seed", "1").c_str());
		m_shake.startMs = nowMs;
		m_shake.active = true;
		m_counters.quake++;
	}

	void FxDriver::OnFlash(const std::map<std::string, std::string>& attrs, int64_t nowMs)
	{
		uint8_t rgba[4] = { 255, 255, 255, 255 };
		const std::string color = AttrOf(attrs, "color", "ffffff");
		if(!artemis::ParseColor(color, rgba))
			rgba[0] = rgba[1] = rgba[2] = 255;
		m_flash.color[0] = rgba[0];
		m_flash.color[1] = rgba[1];
		m_flash.color[2] = rgba[2];
		m_flash.alpha = std::min(255, std::max(0, atoi(AttrOf(attrs, "alpha", "255").c_str())));
		m_flash.timeMs = atoll(AttrOf(attrs, "time", "0").c_str());
		m_flash.loops = std::max(1, atoi(AttrOf(attrs, "loop", "1").c_str()));
		m_flash.startMs = nowMs;
		m_flash.active = true;
		m_counters.flash++;
	}

	int64_t FxDriver::LayerValue(const std::string& id, const std::string& param,
		int64_t nowMs) const
	{
		for(const LayerAnim& anim : m_anims)
		{
			if(anim.id != id || anim.param != param)
				continue;
			const int64_t t = nowMs - anim.startMs;
			if(anim.kind == LayerAnim::Kind::Scetween)
				return artemis::fx::ScetweenValueAt(anim.scetween, anim.base, t);
			if(anim.kind == LayerAnim::Kind::Lytween)
				return artemis::fx::ValueAt(anim.tween, t);
		}
		return 0;
	}

	void FxDriver::ApplyLayers(artemis::Stage& stage, int64_t nowMs)
	{
		for(LayerAnim& anim : m_anims)
		{
			const int64_t t = nowMs - anim.startMs;
			if(anim.kind == LayerAnim::Kind::Scetween)
			{
				int64_t value = artemis::fx::ScetweenValueAt(anim.scetween, anim.base, t);
				if(anim.param == "alpha")
					value = artemis::fx::ClampProperty(value, 0, 255);
				if(stage.HasLayer(anim.id) || value != 0)
					stage.SetProperty(anim.id, anim.param, std::to_string(value));
				continue;
			}
			if(anim.kind == LayerAnim::Kind::Lytween)
			{
				int64_t value = artemis::fx::ValueAt(anim.tween, t);
				if(anim.param == "alpha")
					value = artemis::fx::ClampProperty(value, 0, 255);
				stage.SetProperty(anim.id, anim.param, std::to_string(value));
				continue;
			}
			// anime: no interpolation between keys, the key that has arrived is on
			anim.lastSample = artemis::fx::SampleAnime(anim.anime, t);
			if(anim.lastSample.index < 0 ||
				anim.lastSample.index >= (int)anim.anime.keys.size())
				continue;
			const artemis::fx::AnimeKey& key = anim.anime.keys[anim.lastSample.index];
			if(key.hasTop)
				stage.SetProperty(anim.id, "top", std::to_string(key.top));
			if(key.hasLeft)
				stage.SetProperty(anim.id, "left", std::to_string(key.left));
			if(key.hasAlpha)
				stage.SetProperty(anim.id, "alpha", std::to_string(key.alpha));
		}
	}

	int FxDriver::Gain(int64_t nowMs) const
	{
		if(!m_fade.active)
			return artemis::fx::kBrightnessIdentity;
		return artemis::fx::GainAt(nowMs - m_fade.startMs, m_fade.from, m_fade.to,
			m_fade.timeMs);
	}

	artemis::fx::QuakeOffset FxDriver::QuakeOffset(int64_t nowMs) const
	{
		if(!m_shake.active)
			return artemis::fx::QuakeOffset();
		return artemis::fx::QuakeAt(m_shake.quake, nowMs - m_shake.startMs);
	}

	void FxDriver::ApplyCanvas(artemis::Canvas& canvas, int64_t nowMs) const
	{
		const int gain = Gain(nowMs);
		if(gain != artemis::fx::kBrightnessIdentity)
			artemis::fx::ApplyGain(canvas.data(), (size_t)canvas.width() * canvas.height(), gain);
		if(!m_flash.active)
			return;
		const int64_t total = m_flash.timeMs * m_flash.loops;
		const int64_t t = nowMs - m_flash.startMs;
		if(t < 0 || t >= total)
			return;
		// each loop is one full alpha -> 0 ramp
		const int64_t into = m_flash.timeMs > 0 ? t % m_flash.timeMs : 0;
		const double p = m_flash.timeMs > 0
			? 1.0 - (double)into / (double)m_flash.timeMs
			: 0.0;
		uint8_t rgba[4] = { m_flash.color[0], m_flash.color[1], m_flash.color[2],
			(uint8_t)((double)m_flash.alpha * p) };
		canvas.Fill(0, 0, canvas.width(), canvas.height(), rgba);
	}

	bool FxDriver::Active(int64_t nowMs) const
	{
		if(!m_anims.empty())
			return true;
		if(m_fade.active && nowMs - m_fade.startMs < m_fade.timeMs)
			return true;
		if(m_shake.active && QuakeOffset(nowMs).active())
			return true;
		if(m_flash.active && nowMs - m_flash.startMs < m_flash.timeMs * m_flash.loops)
			return true;
		return false;
	}

	// One line per track, for the report and for the self test: what the script
	// asked for, what answered, how long it is, how loud.
	std::vector<std::string> DescribeAudioTracks(const AudioBridge& bridge)
	{
		std::vector<std::string> out;
		for(const AudioTrackReport& track : bridge.tracks())
		{
			char line[512];
			snprintf(line, sizeof line,
				"%-5s %-22s -> %-28s %6.2fs rms=%.4f peak=%.3f%s%s%s",
				track.kind.c_str(), track.asked.c_str(), track.resolved.c_str(),
				track.seconds, track.rms, track.peak, track.silent ? " SILENT" : "",
				track.loop ? " loop" : "", track.started ? "" : " NOT-STARTED");
			out.push_back(line);
			if(!track.note.empty())
				out.push_back("      note: " + track.note);
		}
		return out;
	}

	// A canvas translated by (dx, dy).  Quake moves the whole picture, so it is
	// applied after compositing and before the words would be measured: a shake
	// that moved only some layers would not be a screen shake.
	void TranslateCanvas(artemis::Canvas& out, const artemis::Canvas& in, int dx, int dy)
	{
		out.Clear(0, 0, 0);
		const int width = in.width();
		const int height = in.height();
		for(int y = 0; y < height; y++)
		{
			const int sourceY = y - dy;
			if(sourceY < 0 || sourceY >= height)
				continue;
			for(int x = 0; x < width; x++)
			{
				const int sourceX = x - dx;
				if(sourceX < 0 || sourceX >= width)
					continue;
				const size_t from = ((size_t)sourceY * width + sourceX) * 4;
				const size_t to = ((size_t)y * width + x) * 4;
				memcpy(out.data() + to, in.data() + from, 4);
			}
		}
	}

	double MeanLuma(const artemis::Canvas& canvas)
	{
		const size_t pixels = (size_t)canvas.width() * canvas.height();
		if(pixels == 0)
			return 0;
		double total = 0;
		const uint8_t* data = canvas.data();
		for(size_t i = 0; i < pixels; i++)
			total += 0.299 * data[i * 4] + 0.587 * data[i * 4 + 1] + 0.114 * data[i * 4 + 2];
		return total / (double)pixels;
	}
}

// ===========================================================================
// the interactive front end: a window, clicks, choices, a sound device
// ===========================================================================
//
// This is where the three layers become a game you can sit in front of.  The
// window itself is `src/render/play/play.cpp` (it owns SDL, the clock and the
// input); what is here is the *host* it asks: the interpreter, the stage, the
// effect driver and the mixer, wired into one object.
//
// The flow is the release's own, not a script this file invents:
//
//   1. `first.iet` boots - styles, macros, the message window - and its last
//      `[jump]` goes to the menu.  That jump is where the boot *ends*, so the
//      target is read out of the boot script rather than hardcoded.
//   2. `main.iet` `*menu` runs and falls through into `*title`, which draws
//      `bgimage/main` and seven `[button]`s, then `[stop]`s.  A stop is not an
//      end: the screen stays up and its buttons are clickable.
//   3. The start button says `file="scenario/scene.iet" label="start"`, so a
//      click on it is a jump into the story - and the story's pages advance on
//      clicks like any other galgame.
//
// Choices are the one place where a real title is not enough: this corpus' scene
// has no choice point at all, and the engine's choice UI lives in Lua
// (`calllua function=select_*` in system_script.asb).  `[select]` is therefore
// modelled from its own attributes - the option labels of the named script - and
// drawn in the game's own `select` message style; the self test proves it
// end-to-end with a fixture written for the purpose.
//
// One modelling note that belongs in every report: a `[button ... call="1"]`
// is run as a *jump*.  The interpreter has no public call entry, so "call and
// come back" is not modelled; the title's start button is call="0" and does not
// depend on it.

namespace
{
	struct PlayRun
	{
		bool ok = false;
		std::string error;
		artemis::PlayStats stats;
		std::string bootSummary;
		std::string menuTarget;
		size_t regions = 0;
		size_t points = 0;
		std::vector<std::string> regionText;
		int pages = 0;
		size_t choiceTotal = 0;
		int chosen = -1;
		std::vector<std::string> notes;
		std::vector<std::string> audioLines;
		double audioRms = 0;
		double audioPeak = 0;
		long long audioFrames = 0;
		bool audioSilent = true;
		int msgspeed = 30;
		int width = 0;
		int height = 0;
		int fps = 60;
	};

	// What the boot script's own final [jump] says: the menu's file and label.
	bool BootMenuTarget(const artemis::Vm& vm, std::string& file, std::string& label)
	{
		const artemis::IetScript* boot = vm.script("first.iet");
		if(boot == nullptr)
			return false;
		const artemis::Item* last = nullptr;
		for(const artemis::Item& item : boot->items())
			if(item.kind == artemis::Item::Kind::Command && item.text == "jump")
				last = &item;
		if(last == nullptr)
			return false;
		file = last->AttrOr("file", std::string());
		label = last->AttrOr("label", std::string());
		return !label.empty();
	}

	class InteractiveHost : public artemis::PlayHost
	{
	public:
		InteractiveHost() : m_scratch(1, 1) {}
		bool Open(const Options& options, std::string& error);

		artemis::PlayHost::Step Next() override;
		void SetPageText(const std::string& shown, const std::string& speaker) override
		{
			m_page.text = shown;
			m_page.speaker = speaker;
			m_stage->SetPage(m_page);
		}
		std::string Reveal(const std::string& text, int64_t ms) override;
		int64_t RevealMs(const std::string& text) override;
		void Compose(artemis::Canvas& canvas, int64_t nowMs) override;
		void OnFrame(int64_t nowMs) override
		{
			m_now = nowMs;
			m_sink.now = nowMs;
		}

		int RegionCount() const override { return (int)m_vm->clickRegions().size(); }
		bool RegionRect(int index, int& left, int& top, int& width, int& height) const override
		{
			return m_vm->RegionBox(index, left, top, width, height);
		}
		bool RegionHit(int index, int x, int y) const override
		{
			return m_vm->RegionHit(index, x, y);
		}
		void ActivateRegion(int index) override;

		int ChoiceCount() const override { return (int)m_choices.size(); }
		bool ChoiceRect(int index, int& left, int& top, int& width, int& height) const override;
		void Choose(int index) override;
		void MixAudio(float* out, int frames) override;

		// for the report
		const artemis::Vm& vm() const { return *m_vm; }
		const artemis::Stage& stage() const { return *m_stage; }
		const std::string& bootSummary() const { return m_vm->bootSummary(); }
		const std::string& menuTarget() const { return m_menuTarget; }
		const std::vector<std::string>& notes() const { return m_notes; }
		const std::vector<std::string>& audioLines() const { return m_audioLines; }
		int msgspeed() const { return m_msgspeed; }
		size_t choiceTotal() const { return m_choiceTotal; }
		int chosen() const { return m_chosen; }
		double audioRms() const { return m_audioRms; }
		double audioPeak() const { return m_audioPeak; }
		long long audioFrames() const { return m_audioFrames; }
		int width() const { return m_stage->width(); }
		int height() const { return m_stage->height(); }
		int fps() const { return m_fps; }

	private:
		struct Sink : artemis::Vm::TagSink
		{
			AudioBridge* audio = nullptr;
			FxDriver* fx = nullptr;
			artemis::Stage* stage = nullptr;
			std::vector<std::string>* notes = nullptr;
			int64_t now = 0;

			void OnTag(const artemis::Vm::TagEvent& tag) override
			{
				if(fx != nullptr)
					fx->OnTag(tag, now, stage);
				if(audio != nullptr && audio->open())
				{
					std::string note;
					if(!audio->OnTag(tag.name, tag.attributes, note) && !note.empty() &&
						notes != nullptr && notes->size() < 24)
						notes->push_back(tag.file + ":" + std::to_string(tag.line) + ": " + note);
				}
			}
		};

		struct ChoiceView
		{
			std::string text;
			int left = 0;
			int top = 0;
			int width = 0;
			int height = 0;
		};

		Options m_options;
		artemis::AssetSource m_assets;
		artemis::TextureCache m_textures;
		artemis::FontLibrary m_fonts;
		std::unique_ptr<artemis::Stage> m_stage;
		std::unique_ptr<artemis::Vm> m_vm;
		FxDriver m_fx;
		AudioBridge m_audio;
		bool m_audioOpen = false;
		Sink m_sink;
		artemis::PageText m_page;
		artemis::Canvas m_scratch;
		std::string m_revealText;
		artemis::fx::Text m_segmented;
		int m_msgspeed = 30;
		int m_fps = 60;
		std::string m_menuTarget;
		bool m_inMenu = false;
		std::vector<ChoiceView> m_choices;
		size_t m_choiceTotal = 0;
		int m_chosen = -1;
		std::vector<std::string> m_notes;
		std::vector<std::string> m_audioLines;
		double m_audioRms = 0;
		double m_audioPeak = 0;
		long long m_audioFrames = 0;
		long long m_audioSamples = 0;
		int64_t m_now = 0;
	};

	bool InteractiveHost::Open(const Options& options, std::string& error)
	{
		m_options = options;
		if(!options.archive.empty() && !m_assets.AddArchive(options.archive, error))
			return false;
		if(!options.dataDir.empty() && !m_assets.AddDirectory(options.dataDir, error))
			return false;
		if(m_assets.empty())
		{
			error = "no data: pass --data-dir <extracted> and/or --archive <root.pfs>";
			return false;
		}
		StageSettings settings;
		{
			std::vector<uint8_t> bytes;
			std::string resolved, readError;
			if(m_assets.Read("system.ini", bytes, resolved, readError))
				settings = ReadSystemIni(artemis::DecodeScenarioText(bytes.data(), bytes.size()));
		}
		if(options.haveSize)
		{
			settings.width = options.width;
			settings.height = options.height;
			settings.fromIni = false;
		}
		m_fps = settings.fps;
		m_stage.reset(new artemis::Stage(settings.width, settings.height));
		m_stage->SetFonts(&m_fonts);
		m_scratch = artemis::Canvas(settings.width, settings.height);

		artemis::Vm::Config config;
		config.assets = &m_assets;
		config.stage = m_stage.get();
		config.cache = &m_textures;
		config.fonts = &m_fonts;
		config.bootFile = options.boot;
		config.verbose = options.verbose;
		m_vm.reset(new artemis::Vm(config));
		if(!m_vm->Boot(error))
		{
			error = "boot: " + error;
			return false;
		}

		// the fonts the game's own [font] blocks named
		for(const std::string& name : m_stage->styleNames())
		{
			const std::string face = m_stage->Style(name).face;
			if(face.empty() || m_fonts.Has(face))
				continue;
			std::vector<uint8_t> bytes;
			std::string resolved, readError;
			if(!m_assets.Read(face, bytes, resolved, readError))
				continue;
			std::string fontError;
			if(!m_fonts.Load(face, bytes, fontError) && options.verbose)
				fprintf(stderr, "agame: font %s: %s\n", face.c_str(), fontError.c_str());
		}

		// the game's own milliseconds per glyph
		const std::string msgspeed = m_vm->Get("g.msgspeed");
		if(!msgspeed.empty())
			m_msgspeed = atoi(msgspeed.c_str());
		m_fx.SetMsgspeed(m_msgspeed);
		m_fx.SetTypewriter(true);

		if(options.audio || !options.audioCapture.empty())
		{
			AudioBridge::Config audioConfig;
			audioConfig.assets = &m_assets;
			audioConfig.workDir = "/tmp/agame-audio";
			std::string audioError;
			if(m_audio.Open(audioConfig, audioError))
			{
				m_audioOpen = true;
				m_sink.audio = &m_audio;
			}
			else
			{
				m_notes.push_back("audio: " + audioError);
				fprintf(stderr, "agame: audio: %s\n", audioError.c_str());
			}
		}
		m_sink.fx = &m_fx;
		m_sink.stage = m_stage.get();
		m_sink.notes = &m_notes;
		m_vm->SetTagSink(&m_sink);

		// where the boot's own last [jump] goes: the menu
		std::string menuFile, menuLabel;
		if(BootMenuTarget(*m_vm, menuFile, menuLabel) && options.startAtMenu)
		{
			m_menuTarget = (menuFile.empty() ? std::string("(this file)") : menuFile) + ":" +
				menuLabel;
			if(!m_vm->Start(menuFile, menuLabel, error))
			{
				error = "menu: " + error;
				return false;
			}
			m_inMenu = true;
		}
		else
		{
			const std::string script = options.script.empty() ? "scenario/scene.iet"
				: options.script;
			if(!m_vm->Start(script, options.label, error))
				return false;
			m_menuTarget = "(boot jump skipped: straight to " + script + ")";
		}
		return true;
	}

	artemis::PlayHost::Step InteractiveHost::Next()
	{
		for(int guard = 0; guard < 1000000; guard++)
		{
			const artemis::Vm::Event event = m_vm->Next();
			if(event.kind == artemis::Vm::Event::Kind::Page)
			{
				if(event.speaker.empty() && event.text.empty())
					continue;
				if(m_options.verbose)
					fprintf(stderr, "[page] %s:%d speaker=\"%s\" %zu chars\n",
						event.file.c_str(), event.line, event.speaker.c_str(),
						event.text.size());
				artemis::PlayHost::Step step;
				step.kind = artemis::PlayHost::Step::Kind::Page;
				step.text = event.text;
				step.speaker = event.speaker;
				return step;
			}
			if(event.kind == artemis::Vm::Event::Kind::Select)
			{
				// the options are on screen: draw them and wait for the pick
				m_choices.clear();
				const artemis::TextStyle& style = m_stage->Style("select");
				const int rowHeight = std::max(20, style.size + style.lineSpacing + 16);
				for(size_t i = 0; i < event.choices.size(); i++)
				{
					ChoiceView view;
					view.text = event.choices[i].text;
					view.left = 0;
					view.top = style.top + (int)i * rowHeight;
					view.width = m_stage->width();
					view.height = rowHeight;
					m_choices.push_back(view);
				}
				m_choiceTotal = m_choices.size();
				artemis::PlayHost::Step step;
				step.kind = artemis::PlayHost::Step::Kind::Idle;
				return step;
			}
			if(event.kind == artemis::Vm::Event::Kind::Stop)
			{
				// [stop] parks the script: the screen stays, its buttons are live
				if(m_options.verbose)
					fprintf(stderr, "[idle] %s:%d [stop], %zu click region(s)\n",
						event.file.c_str(), event.line, m_vm->clickRegions().size());
				artemis::PlayHost::Step step;
				step.kind = artemis::PlayHost::Step::Kind::Idle;
				return step;
			}
			if(event.kind == artemis::Vm::Event::Kind::Error)
			{
				artemis::PlayHost::Step step;
				step.kind = artemis::PlayHost::Step::Kind::Error;
				step.error = event.file + ":" + std::to_string(event.line) + ": " + event.error;
				return step;
			}
		}
		artemis::PlayHost::Step step;
		step.kind = artemis::PlayHost::Step::Kind::End;
		return step;
	}

	std::string InteractiveHost::Reveal(const std::string& text, int64_t ms)
	{
		if(text != m_revealText)
		{
			m_revealText = text;
			m_segmented = artemis::fx::Segment(text);
		}
		return artemis::fx::RevealAt(m_segmented, ms, m_msgspeed).shown;
	}

	int64_t InteractiveHost::RevealMs(const std::string& text)
	{
		if(text != m_revealText)
		{
			m_revealText = text;
			m_segmented = artemis::fx::Segment(text);
		}
		return artemis::fx::RevealDurationMs(m_segmented, m_msgspeed);
	}

	void InteractiveHost::ActivateRegion(int index)
	{
		const std::vector<artemis::Vm::ClickRegion>& regions = m_vm->clickRegions();
		if(index < 0 || index >= (int)regions.size())
			return;
		const artemis::Vm::ClickRegion& region = regions[index];
		if(region.call && m_notes.size() < 32)
			m_notes.push_back("button " + region.id + " asks for call=\"1\": run as a jump, "
				"the return is not modelled");
		std::string error;
		m_vm->JumpTo(region.file, region.label, error);
		if(!error.empty())
			m_notes.push_back("button " + region.id + ": " + error);
		else if(m_notes.size() < 32)
			m_notes.push_back("button " + region.id + " -> " +
				(region.file.empty() ? std::string("(this file)") : region.file) + ":" +
				region.label);
		m_inMenu = false;
	}

	bool InteractiveHost::ChoiceRect(int index, int& left, int& top, int& width,
		int& height) const
	{
		if(index < 0 || index >= (int)m_choices.size())
			return false;
		left = m_choices[index].left;
		top = m_choices[index].top;
		width = m_choices[index].width;
		height = m_choices[index].height;
		return true;
	}

	void InteractiveHost::Choose(int index)
	{
		if(index < 0 || index >= (int)m_choices.size())
			return;
		if(m_choices[index].text.size() > 40)
			m_choices[index].text = m_choices[index].text.substr(0, 40);
		const std::string text = m_choices[index].text;
		std::string error;
		if(!m_vm->ResolveChoice(index, error))
			m_notes.push_back("choice: " + error);
		else if(m_notes.size() < 32)
			m_notes.push_back("chose option " + std::to_string(index + 1) + ": " + text);
		m_chosen = index + 1;
		m_choices.clear();
	}

	void InteractiveHost::Compose(artemis::Canvas& canvas, int64_t nowMs)
	{
		m_fx.ApplyLayers(*m_stage, nowMs);
		std::string error;
		m_stage->Composite(canvas, true, error);
		// choices on top, in the game's own `select` message style
		if(!m_choices.empty())
		{
			const artemis::TextStyle base = m_stage->Style("select");
			for(const ChoiceView& view : m_choices)
			{
				artemis::TextStyle row = base;
				row.top = 0;
				row.height = view.height;
				artemis::RgbaImage bitmap;
				int glyphs = 0;
				if(!m_fonts.Render(row, view.text, bitmap, glyphs, error))
					continue;
				canvas.Blend(bitmap.rgba.data(), bitmap.width, bitmap.height, view.left,
					view.top, 255);
			}
		}
		m_fx.ApplyCanvas(canvas, nowMs);
		const int dx = m_fx.Dx(nowMs);
		const int dy = m_fx.Dy(nowMs);
		if(dx != 0 || dy != 0)
		{
			// the whole picture moves, so it moves after compositing
			TranslateCanvas(m_scratch, canvas, dx, dy);
			memcpy(canvas.data(), m_scratch.data(), canvas.byteSize());
		}
	}

	void InteractiveHost::MixAudio(float* out, int frames)
	{
		if(!m_audioOpen)
		{
			memset(out, 0, (size_t)frames * 2 * sizeof(float));
			return;
		}
		m_audio.Mix(out, frames);
		m_audioFrames += frames;
		double sum = 0;
		for(int i = 0; i < frames * 2; i++)
		{
			const double sample = (double)out[i];
			sum += sample * sample;
			const double magnitude = std::fabs(sample);
			if(magnitude > m_audioPeak)
				m_audioPeak = magnitude;
		}
		const long long samples = (long long)frames * 2;
		m_audioRms = std::sqrt((m_audioRms * m_audioRms * (double)m_audioSamples + sum) /
			(double)(m_audioSamples + samples));
		m_audioSamples += samples;
	}

	PlayRun RunInteractive(const Options& options);
}

static PlayRun RunInteractiveImpl(const Options& options)
{
	PlayRun run;
	InteractiveHost host;
	std::string error;
	if(!host.Open(options, error))
	{
		run.error = error;
		return run;
	}
	run.width = host.width();
	run.height = host.height();
	run.fps = host.fps();
	run.bootSummary = host.bootSummary();
	run.menuTarget = host.menuTarget();
	run.msgspeed = host.msgspeed();

	artemis::PlayOptions play;
	play.width = host.width();
	play.height = host.height();
	play.fps = host.fps();
	play.maxPages = options.maxPages > 0 ? options.maxPages : 100;
	play.realTime = options.realTime;
	play.vsync = options.vsync;
	play.audio = options.audio || !options.audioCapture.empty();
	play.script = options.autoplayKeys;
	play.shotTitle = options.shotTitle;
	play.shotScene = options.shotScene;
	play.shotDir = options.shotDir;
	play.quitAfterPages = options.quitAfterPages;
	play.hashFrames = options.hashFrames;
	play.maxFrames = options.realTime ? host.fps() * 60 * 10 : 4000;
	play.capturePath = options.audioCapture;
	play.captureSeconds = options.audioSeconds;

	artemis::Player player;
	if(!player.Open(play, host, error))
	{
		run.error = error;
		return run;
	}
	run.stats = player.Run(error);
	player.Close();
	if(!error.empty())
	{
		run.error = error;
		return run;
	}
	run.ok = true;
	run.pages = run.stats.pages;
	run.regions = host.vm().clickRegions().size();
	run.choiceTotal = host.choiceTotal();
	run.chosen = host.chosen();
	run.notes = host.notes();
	run.audioLines = host.audioLines();
	run.audioRms = host.audioRms();
	run.audioPeak = host.audioPeak();
	run.audioFrames = host.audioFrames();
	run.audioSilent = host.audioPeak() < 1.0 / 32768.0;
	for(size_t i = 0; i < host.vm().clickRegions().size() && i < 10; i++)
	{
		const artemis::Vm::ClickRegion& region = host.vm().clickRegions()[i];
		int left = 0, top = 0, width = 0, height = 0;
		host.vm().RegionBox((int)i, left, top, width, height);
		char line[384];
		snprintf(line, sizeof line, "    region %-4s %4d,%-4d %3dx%-3d -> %s:%s%s",
			region.id.empty() ? "(point)" : region.id.c_str(), left, top, width, height,
			region.file.empty() ? "(this file)" : region.file.c_str(), region.label.c_str(),
			region.call ? " (call=1, run as a jump)" : "");
		run.regionText.push_back(line);
		if(region.id.empty())
			run.points++;
	}
	return run;
}

namespace
{
	PlayRun RunInteractive(const Options& options) { return RunInteractiveImpl(options); }

	void PrintPlayReport(const PlayRun& run)
	{
		const artemis::PlayStats& stats = run.stats;
		printf("\n--- the window ---------------------------------------------------------\n");
		printf("stage: %dx%d at fps=%d (from the game's own system.ini)\n", run.width,
			run.height, run.fps);
		printf("video driver: %s; audio driver: %s (%s)\n", stats.videoDriver.c_str(),
			stats.audioDriver.c_str(),
			stats.audioDevice ? "device open" : "no device - mixing in the loop");
		if(!stats.audioFallback.empty())
			printf("audio fallback: %s\n", stats.audioFallback.c_str());
		printf("clock: %s\n", stats.realTime ? "wall clock, paced" : "virtual (1/60 s per "
			"frame, no sleeping)");
		printf("%s\n", run.bootSummary.c_str());
		printf("menu entry: %s\n", run.menuTarget.c_str());
		printf("g.msgspeed: %d ms per glyph\n", run.msgspeed);

		printf("\n--- what happened ------------------------------------------------------\n");
		printf("pages shown: %d, frames: %d, advances: %d, clicks: %d, keys: %d\n",
			stats.pages, stats.frames, stats.advances, stats.clicks, stats.keys);
		printf("clicks that finished the typing instead of advancing: %d\n",
			stats.completedReveals);
		printf("idle screen reached: %s; script ended: %s\n", stats.idleSeen ? "yes" : "no",
			stats.ended ? "yes" : "no");
		printf("click regions: %zu (%zu from [point])\n", run.regions, run.points);
		for(const std::string& line : run.regionText)
			printf("%s\n", line.c_str());
		if(run.choiceTotal > 0)
			printf("choices offered: %zu; chosen: %d\n", run.choiceTotal, run.chosen);

		printf("\n--- timing -------------------------------------------------------------\n");
		printf("frame time: avg %.3f ms, p50 %.3f ms, p95 %.3f ms, max %.3f ms over %d "
			"frame(s)\n", stats.frameAvgMs, stats.frameP50Ms, stats.frameP95Ms,
			stats.frameMaxMs, stats.frames);
		printf("click -> page: avg %.2f ms, max %.2f ms over %d sample(s)\n",
			stats.latencyAvgMs, stats.latencyMaxMs, stats.latencySamples);
		printf("wall time: %.1f ms; peak RSS: %.1f MB\n", stats.wallMs,
			(double)stats.peakRss / (1024.0 * 1024.0));

		printf("\n--- sound --------------------------------------------------------------\n");
		printf("frames mixed: %lld; rms=%.4f peak=%.3f%s\n", run.audioFrames, run.audioRms,
			run.audioPeak, run.audioSilent ? " SILENT" : "");
		for(const std::string& line : run.audioLines)
			printf("    %s\n", line.c_str());
		if(!run.notes.empty())
		{
			printf("notes:\n");
			for(size_t i = 0; i < run.notes.size() && i < 12; i++)
				printf("    %s\n", run.notes[i].c_str());
		}
		if(!stats.frameHashes.empty())
		{
			uint64_t fold = 0;
			for(uint64_t hash : stats.frameHashes)
				fold = fold * 1099511628211ull + hash;
			printf("frame hash chain: %016llx over %zu frame(s)\n",
				(unsigned long long)fold, stats.frameHashes.size());
		}
		for(const std::string& shot : stats.shots)
			printf("screenshot: %s\n", shot.c_str());
		if(!stats.titleShot.empty())
			printf("screenshot (idle screen): %s\n", stats.titleShot.c_str());
		if(!stats.sceneShot.empty())
			printf("screenshot (first story page): %s\n", stats.sceneShot.c_str());
	}
}

static ScreenRun PlayImpl(const Options& options, bool quiet)
{
	ScreenRun run;
	const Clock::time_point processStart = Clock::now();

	artemis::AssetSource assets;
	std::string error;
	if(!options.archive.empty() && !assets.AddArchive(options.archive, error))
	{
		run.error = error;
		return run;
	}
	if(!options.dataDir.empty() && !assets.AddDirectory(options.dataDir, error))
	{
		run.error = error;
		return run;
	}
	if(assets.empty())
	{
		run.error = "no data: pass --archive <root.pfs> and/or --data-dir <extracted>";
		return run;
	}

	// the scenario may be named as an archive member ("scenario/scene.iet", the
	// strongest form: everything comes out of the archive) or as a file on disk,
	// in which case its directory joins the search behind the archive
	std::string scriptKey = options.script.empty() ? "scenario/scene.iet" : options.script;
	{
		struct stat info;
		if(!options.script.empty() && stat(options.script.c_str(), &info) == 0 &&
			S_ISREG(info.st_mode))
		{
			const size_t slash = options.script.rfind('/');
			const std::string directory = slash == std::string::npos
				? std::string(".")
				: options.script.substr(0, slash);
			assets.AddDirectory(directory, error);
			scriptKey = slash == std::string::npos
				? options.script
				: options.script.substr(slash + 1);
		}
	}

	// the stage size is the game's own answer, not a constant in this file
	StageSettings settings;
	{
		std::vector<uint8_t> bytes;
		std::string resolved;
		std::string readError;
		if(assets.Read("system.ini", bytes, resolved, readError))
			settings = ReadSystemIni(artemis::DecodeScenarioText(bytes.data(), bytes.size()));
	}

	artemis::Stage stage(settings.width, settings.height);
	artemis::TextureCache textures;
	artemis::FontLibrary fonts;
	stage.SetFonts(&fonts);

	artemis::Vm::Config config;
	config.assets = &assets;
	config.stage = &stage;
	config.cache = &textures;
	config.fonts = &fonts;
	config.bootFile = options.boot;
	config.verbose = options.verbose;
	artemis::Vm vm(config);

	// The tags the interpreter hands over: sound, and (with --fx) the effects.
	// Attached only when asked for, so a plain run still reports every one of
	// these names as "known but not acted on" and its numbers do not move.
	FxDriver fxDriver;
	AudioBridge audio;
	bool audioOpen = false;
	struct Sink : artemis::Vm::TagSink
	{
		FxDriver* fx = nullptr;
		AudioBridge* audio = nullptr;
		artemis::Stage* stage = nullptr;
		int64_t now = 0;
		std::vector<std::string> notes;    // what could not be acted on
		std::vector<std::string> bindings; // setonpush: key code -> script label
		std::vector<std::string> choices;  // select: the branch table offers

		void OnTag(const artemis::Vm::TagEvent& tag) override
		{
			if(tag.name == "setonpush")
			{
				// VK code -> (file, label, handler).  The table is what an input layer
				// would be built from, so printing it is the honest deliverable.
				const std::string key = AttrOf(tag.attributes, "key");
				const std::string file = AttrOf(tag.attributes, "file");
				const std::string label = AttrOf(tag.attributes, "label");
				const std::string handler = AttrOf(tag.attributes, "handler");
				char line[512];
				snprintf(line, sizeof line, "    key=%-4s %-8s -> %s%s%s%s",
					key.empty() ? "?" : key.c_str(), VirtualKeyName(atoi(key.c_str())).c_str(),
					file.empty() ? "(this script)" : file.c_str(),
					label.empty() ? "" : ":", label.c_str(),
					handler.empty() ? "" : (" handler=" + handler).c_str());
				bindings.push_back(line);
				return;
			}
			if(tag.name == "select")
			{
				char line[512];
				snprintf(line, sizeof line, "    select file=%s pre=%s cond=%s",
					AttrOf(tag.attributes, "file").c_str(),
					AttrOf(tag.attributes, "pre").c_str(),
					AttrOf(tag.attributes, "cond").c_str());
				choices.push_back(line);
				return;
			}
			if(fx != nullptr)
				fx->OnTag(tag, now, stage);
			if(audio != nullptr && audio->open())
			{
				std::string note;
				if(!audio->OnTag(tag.name, tag.attributes, note) && !note.empty() &&
					notes.size() < 32)
					notes.push_back(tag.file + ":" + std::to_string(tag.line) + ": " + note);
			}
		}
	} sink;
	if(options.fx)
	{
		sink.fx = &fxDriver;
		sink.stage = &stage;
		fxDriver.SetTypewriter(true);
		const std::string msgspeed = vm.Get("g.msgspeed");
		if(!msgspeed.empty())
			fxDriver.SetMsgspeed(atoi(msgspeed.c_str()));
	}
	if(options.audio || !options.audioCapture.empty())
	{
		AudioBridge::Config audioConfig;
		audioConfig.assets = &assets;
		audioConfig.workDir = "/tmp/agame-audio";
		std::string audioError;
		if(audio.Open(audioConfig, audioError))
		{
			audioOpen = true;
			sink.audio = &audio;
		}
		else
			fprintf(stderr, "agame: audio: %s\n", audioError.c_str());
	}
	// The sink is attached on every run: the input binding table is a property of
	// the release, not of the flags, and a tag handed over is counted in
	// frontEndTags() so nothing is silenced by the hand-off.  The effect and audio
	// drivers behind it are still opt-in.
	vm.SetTagSink(&sink);

	const Clock::time_point bootStart = Clock::now();
	if(!vm.Boot(error))
	{
		run.error = "boot: " + error;
		return run;
	}
	run.bootMs = MsSince(bootStart);
	run.bootSummary = vm.bootSummary();

	// the fonts the game's own [font] blocks named
	const Clock::time_point fontStart = Clock::now();
	for(const std::string& name : stage.styleNames())
	{
		const std::string face = stage.Style(name).face;
		if(face.empty() || fonts.Has(face))
			continue;
		std::vector<uint8_t> bytes;
		std::string resolved;
		std::string readError;
		if(!assets.Read(face, bytes, resolved, readError))
			continue;
		std::string fontError;
		if(!fonts.Load(face, bytes, fontError))
		{
			if(!quiet)
				fprintf(stderr, "agame: font %s: %s\n", face.c_str(), fontError.c_str());
			continue;
		}
		run.fontEvidence.push_back(face + " " + std::to_string(bytes.size()) + " bytes " +
			fonts.PostScriptName(face) + " sha1=" + fonts.Sha1Hex(face));
	}
	if(fonts.faces().empty())
	{
		// a script with no boot file: use the release's own main font if present
		std::vector<uint8_t> bytes;
		std::string resolved;
		std::string readError;
		if(assets.Read("GenShin-Mono-Normal.ttf", bytes, resolved, readError) &&
			fonts.Load("GenShin-Mono-Normal.ttf", bytes, readError))
			run.fontEvidence.push_back("GenShin-Mono-Normal.ttf " +
				std::to_string(bytes.size()) + " bytes " + fonts.PostScriptName(
					"GenShin-Mono-Normal.ttf"));
	}
	run.fontMs = MsSince(fontStart);
	run.fontFaces = fonts.faces();

	if(!vm.Start(scriptKey, options.label, error))
	{
		run.error = error;
		return run;
	}

	artemis::Canvas composite(settings.width, settings.height);
	artemis::Canvas bare(settings.width, settings.height);
	artemis::Canvas previous(settings.width, settings.height);
	bool havePrevious = false;
	int transitionsSeen = 0;
	int fadeWritten = 0;
	if(!options.frames.empty() && mkdir(options.frames.c_str(), 0755) != 0 && errno != EEXIST)
	{
		run.error = "cannot create frame directory " + options.frames;
		return run;
	}

	int written = 0;
	double frameTotal = 0;
	double compositeTotal = 0;
	double censusTotal = 0;
	double pngTotal = 0;
	for(int page = 0; page < options.maxPages; page++)
	{
		artemis::Vm::Event event = vm.Next();
		if(event.kind == artemis::Vm::Event::Kind::Stop)
			break;
		if(event.kind == artemis::Vm::Event::Kind::Error)
		{
			run.error = event.file + ":" + std::to_string(event.line) + ": " + event.error;
			return run;
		}
		if(event.kind != artemis::Vm::Event::Kind::Page)
			continue;

		const Clock::time_point frameStart = Clock::now();
		artemis::PageText text;
		text.text = event.text;
		text.speaker = event.speaker;
		if(options.fx)
		{
			// the page is revealed at g.msgspeed; `--fx-at N` renders the page as it
			// looks N ms in, which is how the still-frame path can show the reveal
			fxDriver.BeginPage(text.text, 0);
			if(options.fxAt >= 0)
				text.text = fxDriver.Reveal((int64_t)options.fxAt).shown;
		}
		stage.SetPage(text);
		std::string compositeError;
		stage.Composite(composite, true, compositeError);
		const size_t ink = stage.lastTextPixels();
		const int glyphs = stage.lastGlyphs();
		const double compositeDoneMs = MsSince(processStart);
		const double compositeUsed = MsSince(frameStart);
		if(run.pages == 0)
			run.firstPageCompositeMs = compositeDoneMs;
		compositeTotal += compositeUsed;

		size_t differing = 0, colors = 0, background = 0;
		double censusUsed = 0;
		if(!run.captured)
		{
			// the census is only needed for the frame that is kept: doing it on
			// every page was most of the first version's 335 ms per frame
			const Clock::time_point measureStart = Clock::now();
			stage.Composite(bare, false, compositeError);
			differing = composite.differingPixels(bare, 0, 0, settings.width, settings.height);
			colors = composite.distinctColors();
			background = RegionColors(composite, 0, 0, settings.width, settings.height / 2);
			censusUsed = MsSince(measureStart);
			censusTotal += censusUsed;
		}

		run.pages++;
		run.textPixels = differing;
		run.inkPixels = ink;
		run.glyphs = glyphs;
		run.frameColors = colors;
		run.backgroundColors = background;
		run.speaker = event.speaker;

		const Clock::time_point writeStart = Clock::now();
		if(!options.frames.empty())
		{
			char path[1024];
			snprintf(path, sizeof path, "%s/%s%03d.png", options.frames.c_str(),
				options.framePrefix.c_str(), written + 1);
			std::string writeError;
			if(!artemis::WritePng(path, settings.width, settings.height, composite.data(),
				writeError))
			{
				run.error = writeError;
				return run;
			}
			written++;
		}
		if(!run.captured && stage.hasBackgroundImage() && differing > 0)
		{
			const Clock::time_point captureWriteStart = Clock::now();
			if(!MakeParent(options.out) || !artemis::WritePng(options.out, settings.width,
				settings.height, composite.data(), error))
			{
				run.error = "cannot write " + options.out + ": " + error;
				return run;
			}
			run.captureCompositeMs = compositeDoneMs;
			run.captureCompositeCpuMs = CpuMs();
			run.captured = true;
			run.capturePath = options.out;
			run.layerDetail = stage.DescribeLayers();
			run.captureSpeaker = event.speaker;
			run.captureTransitionMs = stage.lastTransitionMs();
			run.captureTextPixels = differing;
			run.captureInkPixels = ink;
			run.captureColors = colors;
			run.captureBackgroundColors = background;
			run.captureGlyphs = glyphs;
			run.firstFrameMs = MsSince(processStart);
			run.captureWriteMs = MsSince(captureWriteStart);
			// read the PNG back: the frame is checked from the file, not from hope
			artemis::Image back;
			std::string backError;
			if(artemis::ReadPng(options.out, back, backError))
			{
				run.readBack = true;
				run.pngWidth = back.width;
				run.pngHeight = back.height;
				run.pngColors = artemis::CountColors(back.rgba.data(), back.pixels());
				// the words are the pixels that differ from the same frame without
				// them; measured again on the file's own pixels
				size_t different = 0;
				for(size_t i = 0; i + 3 < back.rgba.size(); i += 4)
					if(back.rgba[i] != bare.data()[i] || back.rgba[i + 1] != bare.data()[i + 1] ||
						back.rgba[i + 2] != bare.data()[i + 2])
						different++;
				run.pngTextPixels = different;
			}
			run.captureExtraMs = MsSince(processStart) - compositeDoneMs;
			run.captureReadBackMs = run.captureExtraMs - run.captureWriteMs;
			if(run.censusMs > 0)
				run.captureReadBackMs -= run.censusMs;
		}
		// a transition since the last page: sample it.  The mix is between the two
		// textless pictures, so the previous page's words do not ghost over the
		// new ones; the words arrive on the final frame.
		if(options.fadeFrames > 0 && !fadeWritten && havePrevious && run.captured &&
			stage.transitionCount() > transitionsSeen && stage.lastTransitionMs() > 0 &&
			!options.frames.empty())
		{
			fadeWritten = options.fadeFrames;
			for(int step = 1; step <= options.fadeFrames; step++)
			{
				const double t = (double)step / (double)(options.fadeFrames + 1);
				artemis::Canvas mixed(settings.width, settings.height);
				const uint8_t* before = previous.data();
				const uint8_t* after = bare.data();
				uint8_t* out = mixed.data();
				for(size_t i = 0; i < mixed.byteSize(); i++)
					out[i] = (uint8_t)(before[i] * (1.0 - t) + after[i] * t);
				char path[1024];
				snprintf(path, sizeof path, "%s/fade%03d.png", options.frames.c_str(), step);
				std::string fadeError;
				artemis::WritePng(path, settings.width, settings.height, mixed.data(), fadeError);
			}
			run.fadeFrames = options.fadeFrames;
			run.fadeMs = stage.lastTransitionMs();
		}
		if(options.fadeFrames > 0)
		{
			// keep this page's picture without its words as the "before" of the
			// next transition
			if(censusUsed > 0)
				previous = bare;
			else
				stage.Composite(previous, false, compositeError);
			havePrevious = true;
		}
		transitionsSeen = stage.transitionCount();
		const double pngUsed = MsSince(writeStart);
		pngTotal += pngUsed;
		const double frameMs = compositeUsed + censusUsed + pngUsed;
		frameTotal += frameMs;
		if(!quiet)
		{
			printf("page %d %s:%d: speaker=\"%s\" textPixels=%zu colors=%zu glyphs=%d "
				"(composite %.2f ms)\n",
				run.pages, event.file.c_str(), event.line, event.speaker.c_str(), differing,
				colors, glyphs, compositeUsed);
			if(!event.text.empty())
				printf("        %s\n", event.text.c_str());
			fflush(stdout);
		}
		run.perFrameMs = frameTotal / run.pages;
	}

	if(run.pages == 0)
	{
		run.error = "the script produced no page of text";
		return run;
	}
	if(audioOpen)
	{
		if(!options.audioCapture.empty())
		{
			std::string captureError;
			if(!audio.Capture(options.audioCapture, options.audioSeconds, captureError))
			{
				run.error = "audio capture: " + captureError;
				return run;
			}
			run.audioCaptured = true;
			run.audioPath = audio.capturedPath();
			run.audioSeconds = audio.capturedSeconds();
			run.audioRms = audio.capturedRms();
			run.audioPeak = audio.capturedPeak();
			run.audioSilent = audio.capturedSilent();
			run.audioFrames = audio.capturedFrames();
		}
		run.audioTracks = audio.tracks().size();
		run.audioLines = DescribeAudioTracks(audio);
		run.audioNotes = audio.notes();
		for(const std::string& note : sink.notes)
			run.audioNotes.push_back(note);
	}
	run.fxTags = (size_t)(fxDriver.counters().scetween + fxDriver.counters().lytween +
		fxDriver.counters().anime + fxDriver.counters().sfade + fxDriver.counters().quake);
	run.fxNotes = fxDriver.counters().notes;
	// the binding table as the *scripts declare it*, not only where this run
	// happened to pass: a key map that depends on the route taken is not a map
	run.bindingsExecuted = sink.bindings.size();
	run.frontEndTags = vm.frontEndTags();
	for(const std::string& key : vm.loadedScripts())
	{
		const artemis::IetScript* script = vm.script(key);
		if(script == nullptr)
			continue;
		for(const artemis::Item& item : script->items())
		{
			if(item.kind != artemis::Item::Kind::Command)
				continue;
			if(item.text == "setonpush")
			{
				const char* attributes[] = { "key", "file", "label", "handler" };
				char line[512];
				const std::string file = item.AttrOr("file",
					item.Attr("handler") != nullptr ? "(handler)" : "(this script)");
				snprintf(line, sizeof line, "    key=%-4s %-8s -> %s:%s  [%s:%d]",
					item.AttrOr("key", "?").c_str(),
					VirtualKeyName(atoi(item.AttrOr("key", "0").c_str())).c_str(),
					file.c_str(), item.AttrOr("label", item.AttrOr("handler", "-")).c_str(),
					key.c_str(), item.line);
				run.bindings.push_back(line);
				(void)attributes;
			}
			else if(item.text == "select")
				run.choices.push_back("    select " + item.Describe());
		}
	}
	run.ok = true;
	run.width = settings.width;
	run.height = settings.height;
	run.fps = settings.fps;
	run.sizeFromIni = settings.fromIni;

	const artemis::Vm::Stats& stats = vm.stats();
	run.commands = (size_t)stats.commands;
	run.macroCalls = (size_t)stats.macroCalls;
	run.macroTable = vm.macroCount();
	run.macroDistinctUsed = vm.macroUses().size();
	run.luaTags = (size_t)stats.luaTags;
	run.conditions = (size_t)stats.conditions;
	run.conditionErrors = (size_t)stats.conditionErrors;
	run.waits = (size_t)stats.waits;
	run.waitMs = stats.waitMs;
	run.unknown = (size_t)stats.unknown;
	run.ignored = (size_t)stats.ignored;
	run.inlineTags = (size_t)stats.inlineTags;
	run.assetFailures = (size_t)stats.assetFailures;
	run.assetFailureText = vm.assetFailuresText();
	for(const std::pair<const std::string, long long>& item : vm.unknownTags())
		run.unknownTags.push_back(item);
	for(const std::pair<const std::string, long long>& item : vm.ignoredTags())
		run.ignoredTags.push_back(item);
	for(const std::pair<const std::string, std::string>& item : vm.unknownLocations())
		run.unknownWhere.push_back(item);
	run.loadedScripts = vm.loadedScripts();
	for(const std::string& key : run.loadedScripts)
	{
		const artemis::IetScript* script = vm.script(key);
		if(script == nullptr)
			continue;
		char line[256];
		snprintf(line, sizeof line, "    %-24s %6d %6d %6zu", key.c_str(),
			script->commandCount(), script->textCount(), script->labels().size());
		run.scriptLines.push_back(line);
	}
	run.styles = stage.styleNames();
	run.layerIds = stage.layerIds();
	run.layerCount = stage.layerCount();
	run.cpuTotalMs = CpuMs();
	run.compositeMs = compositeTotal / run.pages;
	run.censusMs = censusTotal;
	run.pngMs = pngTotal;
	run.transitions = stage.transitionCount();
	run.textureHits = textures.hits();
	run.textureMisses = textures.misses();
	run.textureBytes = textures.cachedBytes();
	run.textureReadMs = textures.readMs();
	run.textureDecodeMs = textures.decodeMs();
	run.peakRss = PeakRssBytes();
	if(run.perFrameMs <= 0)
		run.perFrameMs = 0;
	return run;
}

namespace
{
	ScreenRun Play(const Options& options, bool quiet) { return PlayImpl(options, quiet); }
}

static void PrintReport(const ScreenRun& run, const std::string& assetsText)
{
	printf("\n--- files, scripts and commands -----------------------------------------\n");
	printf("assets: %s\n", assetsText.c_str());
	printf("canvas: %dx%d%s, fps=%d\n", run.width, run.height,
		run.sizeFromIni ? " (from the game's system.ini)" : " (default)", run.fps);
	printf("%s\n", run.bootSummary.c_str());
	printf("scripts parsed:\n");
	printf("    %-24s %6s %6s %6s\n", "script", "cmds", "texts", "labels");
	for(const std::string& line : run.scriptLines)
		printf("%s\n", line.c_str());

	printf("\n--- interpreter ---------------------------------------------------------\n");
	printf("commands executed: %zu\n", run.commands);
	printf("pages: %zu\n", run.pages);
	printf("macro table: %zu macros, %zu expansions (this scene used %zu distinct)\n",
		run.macroTable, run.macroCalls, run.macroDistinctUsed);
	printf("lua e:tag calls executed: %zu\n", run.luaTags);
	printf("conditions evaluated: %zu (%zu unreadable)\n", run.conditions, run.conditionErrors);
	printf("waits: %zu totalling %lld ms (not slept: this is a still-frame run)\n", run.waits,
		run.waitMs);
	printf("transitions: %d in this run; the captured frame's transition asked for %d ms%s\n",
		run.transitions, run.captureTransitionMs,
		run.fadeFrames > 0
			? " - sampled as fade frames (this run)"
			: " - drawn at the final state, the transition is not slept");
	printf("inline tags (ruby/rb/rt): %zu\n", run.inlineTags);
	if(!run.frontEndTags.empty())
	{
		size_t total = 0;
		for(const std::pair<const std::string, long long>& item : run.frontEndTags)
			total += (size_t)item.second;
		printf("handed to the front end (sound, effects, input wiring): %zu\n", total);
		for(const std::pair<const std::string, long long>& item : run.frontEndTags)
			printf("    %-14s %5lld\n", item.first.c_str(), item.second);
	}

	printf("\n--- picture ------------------------------------------------------------\n");
	printf("layers: %zu\n    %s\n", run.layerCount, run.layerDetail.c_str());
	printf("message styles from the game: %zu (", run.styles.size());
	for(size_t i = 0; i < run.styles.size(); i++)
		printf("%s%s", i ? "," : "", run.styles[i].c_str());
	printf(")\n");
	printf("first-screen text pixels: %zu (framebuffer diff), %zu (glyph mask), glyphs: %d, "
		"speaker=\"%s\"\n", run.captureTextPixels, run.captureInkPixels, run.captureGlyphs,
		run.captureSpeaker.c_str());
	printf("first-screen colours: %zu, background colours: %zu\n", run.captureColors,
		run.captureBackgroundColors);
	for(const std::string& evidence : run.fontEvidence)
		printf("font: %s\n", evidence.c_str());
	if(run.captured)
		printf("first screen: %s (read back %dx%d, %zu colours, %zu text pixels)\n",
			run.capturePath.c_str(), run.pngWidth, run.pngHeight, run.pngColors,
			run.pngTextPixels);

	printf("\n--- performance --------------------------------------------------------\n");
	printf("boot (%s): %.2f ms (parse + macroadd + styles)\n",
		run.bootSummary.empty() ? "skipped" : "first.iet", run.bootMs);
	printf("font load: %.2f ms (%zu face(s))\n", run.fontMs, run.fontFaces.size());
	printf("asset read: %.2f ms, image decode: %.2f ms (%zu image(s), cache hits %zu)\n",
		run.textureReadMs, run.textureDecodeMs, run.textureMisses, run.textureHits);
	printf("engine: process start -> first page composited: %.2f ms\n",
		run.firstPageCompositeMs);
	printf("engine: process start -> captured frame composited: %.2f ms wall, %.2f ms CPU\n",
		run.captureCompositeMs, run.captureCompositeCpuMs);
	printf("RENDER PER FRAME: %.3f ms average over %d page(s) (composite + glyphs, no disk)\n",
		run.compositeMs, run.pages);
	printf("CAPTURE ONE-OFF: %.2f ms (census %.2f ms + PNG write %.2f ms + read-back+count "
		"%.2f ms)\n", run.captureExtraMs, run.censusMs, run.captureWriteMs,
		run.captureReadBackMs);
	printf("all pages incl. census and PNG writes: %.3f ms/page\n", run.perFrameMs);
	printf("texture cache: %zu hit(s), %zu miss(es), %zu bytes resident\n", run.textureHits,
		run.textureMisses, run.textureBytes);
	printf("total CPU for the whole run: %.2f ms\n", run.cpuTotalMs);
	printf("peak RSS: %.1f MB\n", (double)run.peakRss / (1024.0 * 1024.0));

	if(!run.audioLines.empty() || run.audioCaptured || run.fxTags > 0)
	{
		printf("\n--- sound and effects --------------------------------------------------\n");
		printf("audio tracks started: %zu\n", run.audioTracks);
		for(const std::string& line : run.audioLines)
			printf("    %s\n", line.c_str());
		if(run.audioCaptured)
			printf("audio capture: %s, %.2fs (%lld frames), rms=%.4f, peak=%.3f%s\n",
				run.audioPath.c_str(), run.audioSeconds, run.audioFrames, run.audioRms,
				run.audioPeak, run.audioSilent ? " SILENT" : "");
		for(size_t i = 0; i < run.audioNotes.size() && i < 10; i++)
			printf("    audio note: %s\n", run.audioNotes[i].c_str());
		printf("effect tags acted on: %zu\n", run.fxTags);
		for(size_t i = 0; i < run.fxNotes.size() && i < 8; i++)
			printf("    fx note: %s\n", run.fxNotes[i].c_str());
	}

	if(!run.bindings.empty() || !run.choices.empty())
	{
		printf("\n--- input wiring -------------------------------------------------------\n");
		printf("setonpush bindings declared: %zu (%zu taken during this run)\n",
			run.bindings.size(), run.bindingsExecuted);
		for(size_t i = 0; i < run.bindings.size() && i < 24; i++)
			printf("%s\n", run.bindings[i].c_str());
		if(run.bindings.size() > 24)
			printf("    ... %zu more\n", run.bindings.size() - 24);
		printf("choice points (select): %zu\n", run.choices.size());
		for(size_t i = 0; i < run.choices.size() && i < 8; i++)
			printf("%s\n", run.choices[i].c_str());
	}

	printf("\n--- compatibility ------------------------------------------------------\n");
	printf("unknown tags: %zu (%zu distinct)\n", run.unknown, run.unknownTags.size());
	for(const std::pair<std::string, long long>& item : run.unknownTags)
	{
		std::string where;
		for(const std::pair<std::string, std::string>& note : run.unknownWhere)
			if(note.first == item.first)
			{
				where = note.second;
				break;
			}
		printf("    %-24s %5lld   first at %s\n", item.first.c_str(), item.second, where.c_str());
	}
	printf("known but not acted on: %zu (%zu distinct)\n", run.ignored, run.ignoredTags.size());
	for(const std::pair<std::string, long long>& item : run.ignoredTags)
		printf("    %-24s %5lld\n", item.first.c_str(), item.second);
	if(run.assetFailures > 0)
		printf("asset failures: %zu (%s)\n", run.assetFailures, run.assetFailureText.c_str());
}

// ===========================================================================
// a shipped .ast: --ast-info, and playing one headless
// ===========================================================================
//
// Two different scripts wear the .ast suffix in this tree, and they are not the
// same language:
//
//   tests/sample-scene.ast   `text = { "start" = { "a line" [800] ... } }`
//                            the format docs/AST.md invented, unquoted keys and
//                            bare `[marker]` values.  src/ast.cpp reads it, and
//                            the twelve-frame demonstration depends on it.
//
//   a shipped .ast           a real Lua table: `astver = 2.0` / `ast = { ... }`,
//                            blocks linked by `linknext`, dialogue under
//                            `text = { ja = {...} }`.  src/luaast.cpp reads it,
//                            and src/ast.cpp chokes on it at byte 17742.
//
// So the path is chosen by *what the bytes are*, not by the file extension: try
// ParseLuaAst, and if it accepts the file play it as a shipped script, otherwise
// hand it to the old reader.  Neither reader is a fallback for the other's
// mistakes - each one is authoritative for its own format.

namespace
{
	// defined further down, next to the self test that also uses it
	std::string FindDataDir();

	bool IsDirPath(const std::string& path)
	{
		struct stat info;
		return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
	}

	// One path, or every *.ast directly inside a directory, in name order.  A
	// directory is how the whole corpus gets reported in one run.
	std::vector<std::string> AstFiles(const std::string& path)
	{
		std::vector<std::string> out;
		if(!IsDirPath(path))
		{
			out.push_back(path);
			return out;
		}
		DIR* directory = opendir(path.c_str());
		if(directory == nullptr)
			return out;
		while(struct dirent* entry = readdir(directory))
		{
			const std::string name = entry->d_name;
			if(name.size() > 4 && name.compare(name.size() - 4, 4, ".ast") == 0)
				out.push_back(path + "/" + name);
		}
		closedir(directory);
		std::sort(out.begin(), out.end());
		return out;
	}

	struct AstFile
	{
		std::string path;
		std::string source;
		artemis::LuaAst ast;
	};

	bool LoadAstFile(const std::string& path, AstFile& out, std::string& error)
	{
		out.path = path;
		out.source = ReadFile(path.c_str());
		if(out.source.empty())
		{
			error = "cannot read " + path;
			return false;
		}
		return artemis::ParseLuaAst(out.source, out.ast, error);
	}

	// `--ast-info`: parse and report.  No SDL, no window, no framebuffer, no PNG:
	// this is the mode the adult corpus is allowed to be measured in, and it is
	// the reason the numbers in docs/REALDATA.md can be quoted at all.
	int RunAstInfo(const Options& options)
	{
		const std::vector<std::string> files = AstFiles(options.astInfoPath);
		if(files.empty())
		{
			fprintf(stderr, "agame: %s: no .ast file there\n", options.astInfoPath.c_str());
			return 1;
		}
		artemis::AstInfoOptions info;
		info.showText = options.showText;
		artemis::AstCorpusStats corpus;
		int failures = 0;
		for(const std::string& path : files)
		{
			AstFile file;
			std::string error;
			if(!LoadAstFile(path, file, error))
			{
				printf("file=%s\nok=0\nparse_error=%s\n", path.c_str(), error.c_str());
				corpus.failed++;
				corpus.failures.push_back(path + ": " + error);
				failures++;
				continue;
			}
			const artemis::AstChainInfo chain = artemis::WalkAstChain(file.ast);
			printf("file=%s\n", path.c_str());
			printf("%s", artemis::FormatAstInfo(file.ast, chain, info).c_str());
			artemis::AddToCorpus(corpus, file.ast, chain);
		}
		if(files.size() > 1)
			printf("%s", artemis::FormatAstCorpus(corpus).c_str());
		fflush(stdout);
		return failures == 0 ? 0 : 1;
	}

	// Everything one .ast playback measures, so the report and the self test read
	// the same numbers.
	struct AstPlayback
	{
		bool ok = false;
		std::string error;
		bool shippedLayout = false; // ParseLuaAst accepted it
		artemis::AstChainInfo chain;

		int blocksTotal = 0;
		int blocksWalked = 0;
		int pages = 0;
		int pagesWithText = 0;
		long long commands = 0;
		long long stageOps = 0;
		long long events = 0;
		long long assetFailures = 0;
		long long policySkipped = 0;
		long long unwired = 0;
		long long bookkeeping = 0;

		int width = 0;
		int height = 0;
		size_t textPixels = 0;   // framebuffer diff, words minus no words
		size_t inkPixels = 0;    // the same from the glyph mask's alpha
		size_t colors = 0;       // distinct colours of the worded frame
		size_t bareColors = 0;   // ... of the same frame without the words
		int glyphs = 0;
		size_t lastTextPixels = 0;
		size_t lastInkPixels = 0;
		int lastGlyphs = 0;
		std::string lastSpeaker;
		double compositeMs = 0;
		int composites = 0;

		std::string dataDir;
		bool dataDirGuessed = false;
		std::string fontFace;
		std::string fontSource;
		std::string styleBox;

		std::vector<std::string> frames;
		std::vector<std::string> assetFailureText;
		std::map<std::string, long long> unwiredTags;
		std::map<std::string, std::string> unwiredAt;
		std::map<std::string, long long> policy;
		std::map<std::string, long long> eventKinds;
		std::vector<artemis::SceneEvent> sceneEvents;
		// sound and effects, when they were asked for
		size_t audioTracks = 0;
		bool audioCaptured = false;
		std::string audioPath;
		double audioSeconds = 0;
		double audioRms = 0;
		double audioPeak = 0;
		bool audioSilent = true;
		long long audioFrames = 0;
		std::vector<std::string> audioLines;
		std::vector<std::string> audioNotes;
		long long fxApplied = 0;
		long long quakeFrames = 0;
		int64_t sceneClockMs = 0;
		std::vector<std::string> fxNotes;
	};

	// The face the words are drawn with.  A shipped .ast title's archive carries
	// no .iet and - in the corpus this is checked against - no TTF either, so the
	// lookup is explicit about where the face came from instead of pretending.
	bool LoadAstFont(artemis::FontLibrary& fonts, const artemis::AssetSource& assets,
		const Options& options, AstPlayback& run, std::string& error)
	{
		if(!options.font.empty())
		{
			const std::string bytes = ReadFile(options.font.c_str());
			const size_t slash = options.font.rfind('/');
			const std::string face = slash == std::string::npos
				? options.font
				: options.font.substr(slash + 1);
			if(bytes.empty())
			{
				error = "cannot read the font " + options.font;
				return false;
			}
			std::vector<uint8_t> data(bytes.begin(), bytes.end());
			if(!fonts.Load(face, data, error))
				return false;
			run.fontFace = face;
			run.fontSource = "--font " + options.font;
			return true;
		}
		const char* names[] = { "GenShin-Mono-Normal.ttf", "GenShinGothic-P-Medium.ttf" };
		for(const char* name : names)
		{
			std::vector<uint8_t> data;
			std::string resolved, readError;
			if(!assets.Read(name, data, resolved, readError))
				continue;
			if(!fonts.Load(name, data, error))
				continue;
			run.fontFace = name;
			run.fontSource = "archive (" + resolved + ", " + std::to_string(data.size()) +
				" bytes)";
			return true;
		}
		// the sibling all-ages Artemis title in this tree ships GenShin; borrowing
		// it is a documented fallback, not a silent one
		const std::string corpus = FindDataDir();
		if(!corpus.empty())
		{
			const std::string path = corpus + "/extracted/GenShin-Mono-Normal.ttf";
			const std::string bytes = ReadFile(path.c_str());
			if(!bytes.empty())
			{
				std::vector<uint8_t> data(bytes.begin(), bytes.end());
				if(fonts.Load("GenShin-Mono-Normal.ttf", data, error))
				{
					run.fontFace = "GenShin-Mono-Normal.ttf";
					run.fontSource = "fallback: the all-ages Artemis title (" + path + ")";
					return true;
				}
			}
		}
		error = "no font: pass --font <file> (this archive ships none)";
		return false;
	}

	AstPlayback PlayAst(const std::string& path, const Options& options);

	void PrintAstReport(const AstPlayback& run);
}

static AstPlayback PlayAstImpl(const std::string& path, const Options& options)
{
	AstPlayback run;
	AstFile file;
	if(!LoadAstFile(path, file, run.error))
		return run;
	run.shippedLayout = true;
	run.chain = artemis::WalkAstChain(file.ast);
	run.blocksTotal = (int)file.ast.blocks.size();

	artemis::AssetSource assets;
	std::string error;
	if(!options.archive.empty() && !assets.AddArchive(options.archive, error))
	{
		run.error = error;
		return run;
	}
	if(!options.dataDir.empty())
	{
		if(!assets.AddDirectory(options.dataDir, error))
		{
			run.error = error;
			return run;
		}
		run.dataDir = options.dataDir;
	}
	if(assets.empty())
	{
		// a .ast sitting in <root>/script/ names its images relative to <root>
		const size_t slash = path.rfind('/');
		if(slash != std::string::npos)
		{
			const std::string guess = path.substr(0, slash) + "/..";
			if(IsDirPath(guess))
			{
				assets.AddDirectory(guess, error);
				run.dataDir = guess;
				run.dataDirGuessed = true;
			}
		}
	}
	if(assets.empty())
	{
		run.error = "no data: pass --data-dir <extracted> (the .ast's own root is not next "
			"to it)";
		return run;
	}

	const int width = options.haveSize ? options.width : 1600;
	const int height = options.haveSize ? options.height : 900;
	artemis::Stage stage(width, height);
	artemis::TextureCache textures;
	artemis::FontLibrary fonts;
	stage.SetFonts(&fonts);

	// The message box is this port's default: a shipped .ast title keeps
	// [chgmsg]/[font] in first.iet and these titles ship no .iet at all, so there
	// is nothing to read the box out of.  The report says "default box" rather
	// than implying the title's own geometry.
	artemis::TextStyle& adv = stage.Style("adv");
	adv.size = std::max(12, height * 5 / 100);
	adv.left = width * 4 / 100;
	adv.top = height * 62 / 100;
	adv.width = width * 92 / 100;
	adv.height = height * 32 / 100;
	adv.shadow = true;
	adv.lineSpacing = std::max(2, height / 90);
	artemis::TextStyle& name = stage.Style("name");
	name.size = std::max(11, adv.size - 2);
	name.left = adv.left;
	name.top = adv.top - std::max(24, adv.size + 6);
	name.width = adv.width / 2;
	name.height = adv.size + 12;
	name.shadow = true;
	{
		char box[128];
		snprintf(box, sizeof box, "adv %dpt at %d,%d %dx%d", adv.size, adv.left, adv.top,
			adv.width, adv.height);
		run.styleBox = box;
	}

	if(!LoadAstFont(fonts, assets, options, run, error))
	{
		run.error = error;
		return run;
	}

	if(!options.frames.empty() && mkdir(options.frames.c_str(), 0755) != 0 && errno != EEXIST)
	{
		run.error = "cannot create frame directory " + options.frames;
		return run;
	}

	artemis::AstRun::Config config;
	config.assets = &assets;
	config.cache = &textures;
	config.stage = &stage;
	config.startBlock = options.startBlock;
	config.maxBlocks = options.maxBlocks;
	config.maxPages = options.maxPages > 0 ? options.maxPages : 0;
	artemis::AstRun scene(file.ast, config);

	// sound and effects, opt-in exactly as on the .iet path
	FxDriver fxDriver;
	AudioBridge audioBridge;
	bool audioOpen = false;
	if(options.audio || !options.audioCapture.empty())
	{
		AudioBridge::Config audioConfig;
		audioConfig.assets = &assets;
		audioConfig.workDir = "/tmp/agame-audio";
		std::string audioError;
		if(audioBridge.Open(audioConfig, audioError))
			audioOpen = true;
		else
			fprintf(stderr, "agame: audio: %s\n", audioError.c_str());
	}
	int64_t clockMs = 0;

	artemis::Canvas frame(width, height);
	artemis::Canvas bare(width, height);
	artemis::Canvas shaken(width, height);
	bool firstFrame = true;
	while(true)
	{
		artemis::PageText page;
		bool hasPage = false;
		if(!scene.Step(page, hasPage, error))
		{
			if(!error.empty())
			{
				run.error = error;
				return run;
			}
			break;
		}
		std::vector<artemis::SceneEvent> events = scene.TakeEvents();
		for(const artemis::SceneEvent& event : events)
		{
			run.events++;
			const char* kind = event.kind == artemis::SceneEvent::Kind::Audio ? "audio"
				: event.kind == artemis::SceneEvent::Kind::Effect ? "effect"
				: event.kind == artemis::SceneEvent::Kind::Wait ? "wait"
				: event.kind == artemis::SceneEvent::Kind::Input ? "input"
				: "other";
			run.eventKinds[kind]++;
			run.sceneEvents.push_back(event);
			if(event.kind == artemis::SceneEvent::Kind::Effect || options.fx)
			{
				fxDriver.OnSceneEvent(event, clockMs);
				run.fxApplied++;
			}
			if(audioOpen && event.kind == artemis::SceneEvent::Kind::Audio)
			{
				std::string note;
				if(!audioBridge.OnSceneEvent(event, note) && !note.empty() &&
					run.audioNotes.size() < 16)
					run.audioNotes.push_back(event.command + " -> " + event.file + ": " + note);
			}
			if(event.kind == artemis::SceneEvent::Kind::Wait)
				clockMs += event.Int("time", 0);
		}
		run.sceneClockMs = clockMs;
		if(!hasPage)
			continue;

		stage.SetPage(page);
		const Clock::time_point frameStart = Clock::now();
		fxDriver.ApplyLayers(stage, clockMs);
		const int quakeDx = fxDriver.Dx(clockMs);
		const int quakeDy = fxDriver.Dy(clockMs);
		if(!stage.Composite(frame, true, error))
		{
			run.error = error;
			return run;
		}
		if(quakeDx != 0 || quakeDy != 0)
		{
			// the whole picture moves, so the shift happens after compositing
			TranslateCanvas(shaken, frame, quakeDx, quakeDy);
			memcpy(frame.data(), shaken.data(), frame.byteSize());
			run.quakeFrames++;
		}
		// brightness and flash land on the finished picture; ApplyCanvas is the
		// identity when neither is running
		fxDriver.ApplyCanvas(frame, clockMs);
		run.compositeMs += MsSince(frameStart);
		run.composites++;
		run.pages++;
		run.lastInkPixels = stage.lastTextPixels();
		run.lastGlyphs = stage.lastGlyphs();
		run.lastSpeaker = page.speaker;
		if(!stage.Composite(bare, false, error))
		{
			run.error = error;
			return run;
		}
		run.lastTextPixels = frame.differingPixels(bare, 0, 0, width, height);
		size_t colors = frame.distinctColors();
		size_t bareColors = bare.distinctColors();
		if(firstFrame)
		{
			run.width = width;
			run.height = height;
			run.inkPixels = run.lastInkPixels;
			run.glyphs = run.lastGlyphs;
			run.colors = colors;
			run.bareColors = bareColors;
			run.textPixels = run.lastTextPixels;
			firstFrame = false;
		}
		if(run.lastInkPixels > 0)
			run.pagesWithText++;

		if(!options.frames.empty())
		{
			char framePath[1024];
			snprintf(framePath, sizeof framePath, "%s/%s%03d.png", options.frames.c_str(),
				options.framePrefix.c_str(), run.pages);
			if(!artemis::WritePng(framePath, width, height, frame.data(), error))
			{
				run.error = error;
				return run;
			}
			run.frames.push_back(framePath);
		}
		if(!options.verbose)
		{
			// the words themselves stay out of the log: the corpus is an adult
			// title, so the *measurements* are printed and the text is not
			printf("page %3d block %-12s: text=%zu px ink=%zu glyphs=%d colours=%zu/%zu "
				"speaker=\"%s\"\n", run.pages, scene.currentBlockName().c_str(),
				run.lastTextPixels, run.lastInkPixels, run.lastGlyphs, colors, bareColors,
				page.speaker.empty() ? "-" : page.speaker.c_str());
			if(options.showText)
				printf("        %s\n", page.text.c_str());
			fflush(stdout);
		}
	}

	if(audioOpen)
	{
		if(!options.audioCapture.empty())
		{
			std::string captureError;
			if(!audioBridge.Capture(options.audioCapture, options.audioSeconds, captureError))
			{
				run.error = "audio capture: " + captureError;
				return run;
			}
			run.audioCaptured = true;
			run.audioPath = audioBridge.capturedPath();
			run.audioSeconds = audioBridge.capturedSeconds();
			run.audioRms = audioBridge.capturedRms();
			run.audioPeak = audioBridge.capturedPeak();
			run.audioSilent = audioBridge.capturedSilent();
			run.audioFrames = audioBridge.capturedFrames();
		}
		run.audioTracks = audioBridge.tracks().size();
		run.audioLines = DescribeAudioTracks(audioBridge);
		for(const std::string& note : audioBridge.notes())
			if(run.audioNotes.size() < 16)
				run.audioNotes.push_back(note);
	}
	run.fxNotes = fxDriver.counters().notes;
	if(run.pages == 0)
	{
		run.error = "the chain produced no page of words";
		return run;
	}
	const artemis::AstRun::Stats& stats = scene.stats();
	run.blocksWalked = stats.blocksWalked;
	run.commands = stats.commands;
	run.stageOps = stats.stageOps;
	run.assetFailures = stats.assetFailures;
	run.policySkipped = stats.policySkipped;
	run.bookkeeping = stats.bookkeeping;
	run.assetFailureText = stats.assetFailuresText;
	run.unwiredTags = stats.unwired;
	run.unwiredAt = stats.unwiredAt;
	run.policy = stats.policy;
	if(run.frames.empty())
	{
		// no frames asked for: still name the first page's numbers, which are what
		// the self test asserts on
		run.width = width;
		run.height = height;
	}
	run.ok = true;
	return run;
}

namespace
{
	AstPlayback PlayAst(const std::string& path, const Options& options)
	{
		return PlayAstImpl(path, options);
	}

	void PrintAstReport(const AstPlayback& run)
	{
		printf("\n--- the script --------------------------------------------------------\n");
		printf("layout: %s%s, %d block(s), chain walked %d, ok %d\n",
			run.chain.chained ? "chained" : "arrayed",
			run.chain.chained ? " (linknext)" : " (dialogue external)", run.blocksTotal,
			(int)run.chain.order.size(), run.chain.ok ? 1 : 0);
		if(!run.chain.error.empty())
			printf("chain: %s\n", run.chain.error.c_str());
		printf("data: %s%s\n", run.dataDir.c_str(), run.dataDirGuessed ? " (guessed from the script's own root)" : "");
		printf("font: %s from %s\n", run.fontFace.c_str(), run.fontSource.c_str());
		printf("box: %s (this port's default: a shipped .ast ships no first.iet)\n",
			run.styleBox.c_str());

		printf("\n--- picture ------------------------------------------------------------\n");
		printf("canvas: %dx%d\n", run.width, run.height);
		printf("pages with words: %d of %d walked; %d of them inked\n", run.pages,
			run.blocksWalked, run.pagesWithText);
		printf("first page: text pixels %zu (framebuffer diff), ink %zu (glyph mask), "
			"glyphs %d, colours %zu with words / %zu without, speaker=\"%s\"\n",
			run.textPixels, run.inkPixels, run.glyphs, run.colors, run.bareColors,
			run.lastSpeaker.c_str() == "" ? "-" : run.lastSpeaker.c_str());
		for(size_t i = 0; i < run.frames.size() && i < 3; i++)
			printf("frame: %s\n", run.frames[i].c_str());
		if(run.frames.size() > 3)
			printf("frame: ... %zu more in the frame directory\n", run.frames.size() - 3);
		if(run.composites > 0)
			printf("RENDER PER PAGE: %.3f ms average over %d page(s)\n",
				run.compositeMs / run.composites, run.composites);

		printf("\n--- commands -----------------------------------------------------------\n");
		printf("commands: %lld; layer writes: %lld; events handed to the front end: %lld\n",
			run.commands, run.stageOps, run.events);
		for(const std::pair<const std::string, long long>& item : run.eventKinds)
			printf("    event %-8s %5lld\n", item.first.c_str(), item.second);
		printf("asset failures: %lld\n", run.assetFailures);
		for(size_t i = 0; i < run.assetFailureText.size() && i < 6; i++)
			printf("    %s\n", run.assetFailureText[i].c_str());
		if(run.assetFailureText.size() > 6)
			printf("    ... %zu more\n", run.assetFailureText.size() - 6);
		printf("refused by the image policy (ev/cg/fg art, never read): %lld\n",
			run.policySkipped);
		for(const std::pair<const std::string, long long>& item : run.policy)
			printf("    %-12s %5lld\n", item.first.c_str(), item.second);
		if(!run.audioLines.empty() || !run.audioNotes.empty() || run.fxApplied > 0)
		{
			printf("\n--- sound and effects --------------------------------------------------\n");
			printf("audio tracks started: %zu\n", run.audioTracks);
			for(const std::string& line : run.audioLines)
				printf("    %s\n", line.c_str());
			if(run.audioCaptured)
				printf("audio capture: %s, %.2fs (%lld frames), rms=%.4f, peak=%.3f%s\n",
					run.audioPath.c_str(), run.audioSeconds, run.audioFrames, run.audioRms,
					run.audioPeak, run.audioSilent ? " SILENT" : "");
			for(size_t i = 0; i < run.audioNotes.size() && i < 8; i++)
				printf("    audio note: %s\n", run.audioNotes[i].c_str());
			printf("effect commands sampled: %lld, frames moved by quake: %lld, script clock "
				"%lld ms\n", run.fxApplied, run.quakeFrames, (long long)run.sceneClockMs);
			for(size_t i = 0; i < run.fxNotes.size() && i < 6; i++)
				printf("    fx note: %s\n", run.fxNotes[i].c_str());
		}
		printf("not wired: %lld (%zu distinct)\n", run.unwired, run.unwiredTags.size());
		for(const std::pair<const std::string, long long>& item : run.unwiredTags)
		{
			std::string where;
			std::map<std::string, std::string>::const_iterator found =
				run.unwiredAt.find(item.first);
			if(found != run.unwiredAt.end())
				where = found->second;
			printf("    %-12s %5lld   first at %s\n", item.first.c_str(), item.second,
				where.c_str());
		}
	}
}

// ===========================================================================
// self test
// ===========================================================================

namespace
{
	struct Tally
	{
		int passed = 0;
		int failed = 0;
		int skipped = 0;
	};

	void Pass(Tally& tally, const std::string& name, const std::string& detail)
	{
		tally.passed++;
		printf("PASS %-22s %s\n", name.c_str(), detail.c_str());
	}

	void Fail(Tally& tally, const std::string& name, const std::string& detail)
	{
		tally.failed++;
		printf("FAIL %-22s %s\n", name.c_str(), detail.c_str());
	}

	void Skip(Tally& tally, const std::string& name, const std::string& detail)
	{
		tally.skipped++;
		printf("SKIP %-22s %s\n", name.c_str(), detail.c_str());
	}

	bool DirExists(const std::string& path)
	{
		struct stat info;
		return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
	}

	bool FileExists(const std::string& path)
	{
		struct stat info;
		return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
	}

	// where the corpus lives: the environment, the working directory, or the
	// checkout this file was compiled in.  Nothing is downloaded and nothing is
	// assumed to exist - a missing corpus is a SKIP.
	std::string FindDataDir()
	{
		if(const char* env = getenv("AGAME_DATA"))
			if(DirExists(env))
				return env;
		const char* candidates[] = {
			"../testdata/artemis-nicolan",
			"../../testdata/artemis-nicolan",
			"testdata/artemis-nicolan",
			"/Users/huangyuliang/Documents/gal-engines/testdata/artemis-nicolan",
		};
		for(const char* candidate : candidates)
			if(DirExists(candidate))
				return candidate;
		const std::string source = __FILE__;
		const size_t at = source.rfind("/tools/");
		if(at != std::string::npos)
		{
			const std::string guess = source.substr(0, at) + "/../testdata/artemis-nicolan";
			if(DirExists(guess))
				return guess;
		}
		return std::string();
	}

	// -----------------------------------------------------------------------
	// the effect layer, asserted on pixels and on layer values
	// -----------------------------------------------------------------------
	//
	// Every case samples the *same* effect at two or more times and asserts the
	// difference the script asked for.  The stage is the all-ages title's real
	// background with its real font, so a case that passes says the effect works
	// on a real picture, not on a gradient somebody drew for the test.
	void RunFxCheck(Tally& tally, const std::string& data)
	{
		if(data.empty() || !DirExists(data + "/extracted"))
		{
			const char* names[] = { "fx-msgspeed", "fx-scetween", "fx-lytween",
				"fx-anime", "fx-sfade", "fx-quake" };
			for(const char* name : names)
				Skip(tally, name, "no corpus");
			return;
		}
		const std::string extracted = data + "/extracted";
		artemis::AssetSource assets;
		std::string error;
		assets.AddDirectory(extracted, error);

		// the real g.msgspeed, out of the game's own boot script
		int msgspeed = 0;
		{
			artemis::TextureCache cache;
			artemis::FontLibrary fonts;
			artemis::Stage stage(1024, 698);
			stage.SetFonts(&fonts);
			artemis::Vm::Config config;
			config.assets = &assets;
			config.stage = &stage;
			config.cache = &cache;
			config.fonts = &fonts;
			config.bootFile = "first.iet";
			artemis::Vm vm(config);
			std::string bootError;
			if(vm.Boot(bootError))
				msgspeed = atoi(vm.Get("g.msgspeed").c_str());
		}

		// one stage every case can borrow: the real background, the real TTF
		artemis::TextureCache textures;
		artemis::FontLibrary fonts;
		artemis::Stage stage(1024, 698);
		stage.SetFonts(&fonts);
		{
			std::vector<uint8_t> bytes;
			std::string resolved, readError;
			if(assets.Read("GenShin-Mono-Normal.ttf", bytes, resolved, readError))
				fonts.Load("GenShin-Mono-Normal.ttf", bytes, readError);
		}
		artemis::TextStyle& adv = stage.Style("adv");
		adv.face = "GenShin-Mono-Normal.ttf";
		adv.size = 36;
		adv.left = 42;
		adv.top = 460;
		adv.width = 937;
		adv.height = 250;
		const bool bgLoaded = stage.SetLayerImage("0", "bgimage/main", assets, textures, error);

		// the first line the all-ages title's own scene script says
		std::string line;
		{
			const std::string path = extracted + "/scenario/scene.iet";
			const std::string bytes = ReadFile(path.c_str());
			if(!bytes.empty())
			{
				artemis::IetScript script;
				std::string parseError;
				if(artemis::IetScript::Parse(artemis::DecodeScenarioText(
					(const uint8_t*)bytes.data(), bytes.size()), script, parseError))
					for(const artemis::Item& item : script.items())
						if(item.kind == artemis::Item::Kind::Text && !item.text.empty())
						{
							line = item.text;
							break;
						}
			}
		}

		// 1. g.msgspeed = 30 ms per glyph, and the page grows with time
		{
			FxDriver fx;
			fx.SetTypewriter(true);
			fx.SetMsgspeed(msgspeed > 0 ? msgspeed : 30);
			fx.BeginPage(line.empty() ? "あいうえおかきくけこ" : line, 0);
			const artemis::fx::Reveal at0 = fx.Reveal(0);
			const artemis::fx::Reveal at400 = fx.Reveal(400);
			const int64_t total = fx.pageDurationMs();
			// the reveal must land on a glyph boundary: cutting inside a UTF-8
			// sequence would draw a replacement box
			const artemis::fx::Reveal atEnd = fx.Reveal(total);
			const bool boundary = at0.bytes <= at400.bytes && at400.bytes <= atEnd.bytes &&
				atEnd.bytes == fx.pageSize() && !at400.complete && atEnd.complete;
			size_t pixelsAt0 = 0, pixelsAt400 = 0, pixelsAtEnd = 0;
			{
				artemis::Canvas canvas(1024, 698);
				artemis::Canvas bare(1024, 698);
				stage.Composite(bare, false, error);
				stage.SetPageText(at0.shown);
				stage.Composite(canvas, true, error);
				pixelsAt0 = canvas.differingPixels(bare, 0, 0, 1024, 698);
				stage.SetPageText(at400.shown);
				stage.Composite(canvas, true, error);
				pixelsAt400 = canvas.differingPixels(bare, 0, 0, 1024, 698);
				stage.SetPageText(fx.Reveal(total).shown);
				stage.Composite(canvas, true, error);
				pixelsAtEnd = canvas.differingPixels(bare, 0, 0, 1024, 698);
			}
			const bool ok = msgspeed == 30 && bgLoaded && at0.glyphs == 1 &&
				at400.glyphs > at0.glyphs && !at400.complete && at400.bytes > at0.bytes &&
				boundary && pixelsAt0 < pixelsAt400 && pixelsAt400 < pixelsAtEnd &&
				pixelsAtEnd > 1000;
			char detail[512];
			snprintf(detail, sizeof detail,
				"g.msgspeed=%d; t=0 %zu glyph/%zu px, t=400 %zu glyph/%zu px, end %zu glyph/%zu "
				"px at %lld ms", msgspeed, at0.glyphs, pixelsAt0, at400.glyphs, pixelsAt400,
				atEnd.glyphs, pixelsAtEnd, (long long)total);
			if(ok)
				Pass(tally, "fx-msgspeed", detail);
			else
				Fail(tally, "fx-msgspeed", detail);
			stage.SetPageText(std::string());
		}

		// 2. scetween is a delta from the value the tag arrived at: two in a row
		//    accumulate, which a "from -> to" reading could not do
		{
			FxDriver fx;
			std::string layerError;
			// a different picture from the one below it: an alpha ramp over the same
			// image is invisible, which would make a pixel assertion meaningless
			stage.SetLayerImage("in", "bgimage/milkeyway", assets, textures, layerError);
			stage.SetProperty("in", "alpha", "200");
			std::map<std::string, std::string> tag;
			tag["mode"] = "add";
			tag["type"] = "in";
			tag["param"] = "alpha";
			tag["ease"] = "none";
			tag["diff"] = "-100";
			tag["time"] = "400";
			tag["delay"] = "0";
			fx.OnScetween(tag, 0, &stage);
			int a0 = 0, a200 = 0, a400 = 0;
			fx.ApplyLayers(stage, 0);
			stage.LayerProperty("in", "alpha", a0);
			fx.ApplyLayers(stage, 200);
			stage.LayerProperty("in", "alpha", a200);
			fx.ApplyLayers(stage, 400);
			stage.LayerProperty("in", "alpha", a400);
			// the same tag again, from where the first one ended
			fx.OnScetween(tag, 400, &stage);
			int a800 = 0;
			fx.ApplyLayers(stage, 800);
			stage.LayerProperty("in", "alpha", a800);
			// and the pixels agree: a dimmer layer is a different picture
			artemis::Canvas dim(1024, 698), bright(1024, 698);
			stage.SetProperty("in", "alpha", "200");
			stage.Composite(bright, false, error);
			stage.SetProperty("in", "alpha", "0");
			stage.Composite(dim, false, error);
			const size_t differing = bright.differingPixels(dim, 0, 0, 1024, 698);
			const bool ok = a0 == 200 && a200 == 150 && a400 == 100 && a800 == 0 &&
				fx.counters().scetween == 2 && differing > 1000;
			char detail[512];
			snprintf(detail, sizeof detail,
				"base 200 diff -100: t=%d,%d,%d then a second tag -> %d; %zu px differ "
				"between alpha 200 and 0", a0, a200, a400, a800, differing);
			if(ok)
				Pass(tally, "fx-scetween", detail);
			else
				Fail(tally, "fx-scetween", detail);
			stage.DeleteLayer("in");
		}

		// 3. lytween is absolute and yoyo="-1" runs back and forth; the malformed
		//    tag in the corpus (a `top=` where `to=` belongs) is refused, not guessed
		{
			FxDriver fx;
			std::map<std::string, std::string> tag;
			tag["id"] = "81.0";
			tag["param"] = "top";
			tag["ease"] = "easeinout_quart";
			tag["from"] = "40";
			tag["to"] = "35";
			tag["time"] = "1500";
			tag["yoyo"] = "-1";
			fx.OnLytween(tag, 0);
			int t0 = 0, t1500 = 0, t3000 = 0, t4500 = 0;
			fx.ApplyLayers(stage, 0);
			stage.LayerProperty("81.0", "top", t0);
			fx.ApplyLayers(stage, 1500);
			stage.LayerProperty("81.0", "top", t1500);
			fx.ApplyLayers(stage, 3000);
			stage.LayerProperty("81.0", "top", t3000);
			fx.ApplyLayers(stage, 4500);
			stage.LayerProperty("81.0", "top", t4500);
			// the cycle with real numbers: from, to, from, to again
			const bool cycled = t0 == 40 && t1500 == 35 && t3000 == 40 && t4500 == 35;

			// and the same shape with room to see the middle: 40 -> 0, linear, so the
			// halfway points are not hidden by 5 px of travel and quart easing
			FxDriver wide;
			std::map<std::string, std::string> straight;
			straight["id"] = "wide";
			straight["param"] = "left";
			straight["ease"] = "none";
			straight["from"] = "0";
			straight["to"] = "400";
			straight["time"] = "1000";
			straight["yoyo"] = "-1";
			wide.OnLytween(straight, 0);
			int w500 = 0, w1000 = 0, w1500 = 0, w2000 = 0;
			wide.ApplyLayers(stage, 500);
			stage.LayerProperty("wide", "left", w500);
			wide.ApplyLayers(stage, 1000);
			stage.LayerProperty("wide", "left", w1000);
			wide.ApplyLayers(stage, 1500);
			stage.LayerProperty("wide", "left", w1500);
			wide.ApplyLayers(stage, 2000);
			stage.LayerProperty("wide", "left", w2000);

			// the two broken tags in the real first.iet
			std::map<std::string, std::string> broken;
			broken["id"] = "1";
			broken["param"] = "yscale";
			broken["from"] = "100";
			broken["top"] = "100";
			broken["time"] = "20000";
			broken["sync"] = "0";
			fx.OnLytween(broken, 0);
			const bool ok = cycled && w500 == 200 && w1000 == 400 && w1500 == 200 &&
				w2000 == 0 && fx.counters().lytween == 1 &&
				fx.counters().lytweenRefused == 1;
			char detail[512];
			snprintf(detail, sizeof detail,
				"top 40->35/1500 yoyo=-1: t=0 %d, 1500 %d, 3000 %d, 4500 %d; "
				"left 0->400/1000 linear yoyo: 500 %d, 1000 %d, 1500 %d, 2000 %d; "
				"malformed tag refused=%lld", t0, t1500, t3000, t4500, w500, w1000, w1500,
				w2000, fx.counters().lytweenRefused);
			if(ok)
				Pass(tally, "fx-lytween", detail);
			else
				Fail(tally, "fx-lytween", detail);
			stage.DeleteLayer("wide");
		}

		// 4. anime is a keyframe track, not a tween: the key that has arrived is on
		//    screen and nothing is interpolated between keys
		{
			FxDriver fx;
			struct Key { int top; int left; int alpha; int time; };
			// first.iet's own glyph track for layer 81.1
			static const Key keys[] = {
				{ 40, 0, 250, 0 }, { 40, 3, 200, 100 }, { 40, 5, 150, 200 },
				{ 43, 5, 100, 300 }, { 45, 5, 50, 400 }, { 45, 3, 100, 500 },
			};
			for(int i = 0; i < 6; i++)
			{
				std::map<std::string, std::string> tag;
				tag["id"] = "81.1";
				tag["mode"] = i == 0 ? "init" : "add";
				tag["file"] = "forsystem/glyph";
				tag["top"] = std::to_string(keys[i].top);
				tag["left"] = std::to_string(keys[i].left);
				tag["alpha"] = std::to_string(keys[i].alpha);
				tag["time"] = std::to_string(keys[i].time);
				fx.OnAnime(tag, 0);
			}
			// fx::SampleAnime reads a key's `time` as "how long until this key
			// arrives", so the arrivals here are 100, 300, 600, 1000, 1500 ms
			int top299 = 0, top599 = 0, top600 = 0, top1000 = 0;
			fx.ApplyLayers(stage, 299);
			stage.LayerProperty("81.1", "top", top299);
			fx.ApplyLayers(stage, 599);
			stage.LayerProperty("81.1", "top", top599);
			fx.ApplyLayers(stage, 600);
			stage.LayerProperty("81.1", "top", top600);
			fx.ApplyLayers(stage, 1000);
			stage.LayerProperty("81.1", "top", top1000);
			const bool ok = top299 == 40 && top599 == 40 && top600 == 43 &&
				top1000 == 45 && fx.counters().anime == 6 &&
				fx.counters().animeTracks == 1;
			char detail[256];
			snprintf(detail, sizeof detail,
				"top at t=299,599,600,1000 is %d,%d,%d,%d (40 until 599, 43 at 600 - a "
				"jump, no ramp); %lld keys", top299, top599, top600, top1000,
				fx.counters().anime);
			if(ok)
				Pass(tally, "fx-anime", detail);
			else
				Fail(tally, "fx-anime", detail);
			stage.DeleteLayer("81.1");
		}

		// 5. sfade: gain 1000 is the identity for every channel value, 500 halves
		//    the picture, and a ramp is halfway at half the time
		{
			bool identity = true;
			for(int v = 0; v < 256; v++)
				if(artemis::fx::ScaleChannel((uint8_t)v, 1000) != (uint8_t)v)
					identity = false;
			FxDriver fx;
			artemis::Canvas canvas(1024, 698);
			stage.Composite(canvas, false, error);
			const double full = MeanLuma(canvas);
			fx.ApplyCanvas(canvas, 0); // no fade yet: still the identity
			const bool unchanged = MeanLuma(canvas) == full;
			std::map<std::string, std::string> tag;
			tag["gain"] = "500";
			tag["time"] = "0";
			fx.OnSfade(tag, 0);
			artemis::Canvas half(1024, 698);
			stage.Composite(half, false, error);
			fx.ApplyCanvas(half, 0);
			const double halfLuma = MeanLuma(half);
			// a 2000 ms ramp to black from full brightness, sampled in the middle.
			// A second fade on the *same* driver would start from 500 - which is
			// right, and is why the ramp case gets its own driver.
			FxDriver ramp;
			tag["gain"] = "0";
			tag["time"] = "2000";
			ramp.OnSfade(tag, 0);
			artemis::Canvas mid(1024, 698);
			stage.Composite(mid, false, error);
			ramp.ApplyCanvas(mid, 1000);
			const double midLuma = MeanLuma(mid);
			artemis::Canvas rampEnd(1024, 698);
			stage.Composite(rampEnd, false, error);
			ramp.ApplyCanvas(rampEnd, 2000);
			const double endLuma = MeanLuma(rampEnd);
			const double halfRatio = full > 0 ? halfLuma / full : 0;
			const double midRatio = full > 0 ? midLuma / full : 0;
			const bool ok = identity && unchanged && std::abs(halfRatio - 0.5) < 0.02 &&
				std::abs(midRatio - 0.5) < 0.02 && endLuma < 1.0 &&
				fx.counters().sfade == 1 && ramp.counters().sfade == 1;
			char detail[256];
			snprintf(detail, sizeof detail,
				"gain 1000 identity on 256/256 values; gain 500 -> %.3f of the luma; ramp at "
				"1000/2000 ms -> %.3f, at 2000 -> %.3f of it", halfRatio, midRatio,
				full > 0 ? endLuma / full : 0);
			if(ok)
				Pass(tally, "fx-sfade", detail);
			else
				Fail(tally, "fx-sfade", detail);
		}

		// 6. quake: the same seed shakes the same way, the offset moves the whole
		//    picture, and it stops when the tag's time is up
		{
			FxDriver first, second;
			std::map<std::string, std::string> tag;
			tag["time"] = "50";
			tag["count"] = "2";
			tag["size"] = "30";
			tag["dir"] = "h";
			first.OnQuake(tag, 0);
			second.OnQuake(tag, 0);
			bool same = true;
			bool anyMoved = false;
			for(int t = 0; t <= 50; t++)
			{
				if(first.Dx(t) != second.Dx(t) || first.Dy(t) != second.Dy(t))
					same = false;
				if(first.Dx(t) != 0 || first.Dy(t) != 0)
					anyMoved = true;
			}
			const bool stopped = first.Dx(50) == 0 && first.Dy(50) == 0 &&
				first.Dx(999) == 0;
			artemis::Canvas plain(1024, 698), shaken(1024, 698);
			stage.Composite(plain, false, error);
			const int dx = first.Dx(1), dy = first.Dy(1);
			TranslateCanvas(shaken, plain, dx, dy);
			const size_t moved = plain.differingPixels(shaken, 0, 0, 1024, 698);
			// and the shift is a translation, not a smear: a pixel lands where it was
			const bool translated = std::abs(dy) >= 698 || dx == 0 ||
				plain.At(100, 100) == shaken.At(100 + dx, 100 + dy);
			const bool ok = same && anyMoved && stopped && moved > 0 && translated;
			char detail[256];
			snprintf(detail, sizeof detail,
				"seeded offsets identical across runs=%d, moved=%d, first offset (%d,%d), "
				"%zu px move, quiet at t=50", (int)same, (int)anyMoved, dx, dy, moved);
			if(ok)
				Pass(tally, "fx-quake", detail);
			else
				Fail(tally, "fx-quake", detail);
		}
	}

	// Where the shipped-.ast corpus lives.  Same rule as FindDataDir: the test
	// belongs in the tree, the corpus does not, and a missing one is a SKIP.
	std::string FindAstCorpus()
	{
		if(const char* env = getenv("AGAME_AST"))
			if(DirExists(env))
				return env;
		const char* candidates[] = {
			"../testdata/artemis-henpri-trial/extracted/script",
			"../../testdata/artemis-henpri-trial/extracted/script",
			"testdata/artemis-henpri-trial/extracted/script",
			"/Users/huangyuliang/Documents/gal-engines/testdata/artemis-henpri-trial/extracted/script",
		};
		for(const char* candidate : candidates)
			if(DirExists(candidate))
				return candidate;
		const std::string source = __FILE__;
		const size_t at = source.rfind("/tools/");
		if(at != std::string::npos)
		{
			const std::string guess = source.substr(0, at) +
				"/../testdata/artemis-henpri-trial/extracted/script";
			if(DirExists(guess))
				return guess;
		}
		return std::string();
	}

	int RunSelfTest(const Options& optionsIn)
	{
		Tally tally;
		const std::string data = optionsIn.dataDir.empty() ? FindDataDir() : optionsIn.dataDir;
		printf("agame selftest\n");		printf("data directory: %s\n\n", data.empty() ? "(none found)" : data.c_str());

		// 1. the decoder: the real scripts are CP932, not UTF-8 and not plain JIS
		{
			const uint8_t basic[] = { 0x83, 0x65, 0x83, 0x58, 0x83, 0x67, 0x81, 0x40,
				0x93, 0xfa, 0x96, 0x7b };
			const uint8_t extended[] = { 0x87, 0x40, 0x81, 0x60, 0x87, 0x8a, 0xfb, 0xfc };
			const std::string a = artemis::SjisToUtf8(basic, sizeof basic);
			const std::string b = artemis::SjisToUtf8(extended, sizeof extended);
			const bool ok = a == "テスト　日本" && b == "①～㈱髙";
			if(ok)
				Pass(tally, "sjis-cp932", a + " / " + b);
			else
				Fail(tally, "sjis-cp932", a + " / " + b);
		}

		const std::string scenePath = data + "/extracted/scenario/scene.iet";
		const std::string archivePath = data + "/root.pfs";
		const std::string extracted = data + "/extracted";

		// 2. the real scenario parses: 7 labels, 1066 tags
		if(!data.empty() && FileExists(scenePath))
		{
			const std::string bytes = ReadFile(scenePath.c_str());
			artemis::IetScript script;
			std::string parseError;
			const std::string text = artemis::DecodeScenarioText(
				(const uint8_t*)bytes.data(), bytes.size());
			const bool parsed = artemis::IetScript::Parse(text, script, parseError);
			bool firstLine = false;
			for(const artemis::Item& item : script.items())
				if(item.kind == artemis::Item::Kind::Text &&
					item.text.find("俺がまだ") != std::string::npos)
					firstLine = true;
			const bool ok = parsed && script.labels().size() == 7 &&
				script.commandCount() == 1077 && script.textCount() == 458 && firstLine &&
				script.inlineTagCount() == 33;
			char detail[256];
			snprintf(detail, sizeof detail,
				"labels=%zu commands=%d texts=%d inline=%d firstLine=%d", 
				script.labels().size(), script.commandCount(), script.textCount(),
				script.inlineTagCount(), (int)firstLine);
			if(ok)
				Pass(tally, "parse-scene.iet", detail);
			else
				Fail(tally, "parse-scene.iet", detail + (" " + parseError));
		}
		else
			Skip(tally, "parse-scene.iet", "no corpus: " + scenePath);

		// 3. the boot script really defines the UI: macros and message styles
		Options options;
		options.archive = FileExists(archivePath) ? archivePath : std::string();
		options.dataDir = extracted;
		options.script = "scenario/scene.iet";
		options.boot = "first.iet";
		options.maxPages = 6;
		options.out = "screenshots/real-first-screen.png";

		if(!data.empty() && FileExists(scenePath) && DirExists(extracted))
		{
			artemis::AssetSource assets;
			std::string error;
			if(!options.archive.empty())
				assets.AddArchive(options.archive, error);
			assets.AddDirectory(extracted, error);
			artemis::Stage stage(1024, 698);
			artemis::TextureCache textures;
			artemis::FontLibrary fonts;
			stage.SetFonts(&fonts);
			artemis::Vm::Config config;
			config.assets = &assets;
			config.stage = &stage;
			config.cache = &textures;
			config.fonts = &fonts;
			config.bootFile = "first.iet";
			artemis::Vm vm(config);
			std::string bootError;
			const bool booted = vm.Boot(bootError);
			const bool hasAdv = stage.hasStyle("adv");
			const bool hasName = stage.hasStyle("name");
			const artemis::TextStyle& adv = stage.Style("adv");
			const artemis::TextStyle& nameStyle = stage.Style("name");
			const bool ok = booted && hasAdv && hasName && adv.size == 36 &&
				adv.face == "GenShin-Mono-Normal.ttf" && adv.left == 42 && adv.top == 460 &&
				adv.width == 937 && nameStyle.top == 390 && vm.macroCount() > 80 &&
				vm.macroNames().end() != std::find(vm.macroNames().begin(),
					vm.macroNames().end(), "girl") &&
				vm.Get("g.screen_width") == "1024" && vm.Get("g.msgalpha") == "200";
			char detail[512];
			snprintf(detail, sizeof detail,
				"macros=%zu adv=%dpt/%s/%d,%d %dx%d name.top=%d screen=%sx%s msgalpha=%s",
				vm.macroCount(), adv.size, adv.face.c_str(), adv.left, adv.top, adv.width,
				adv.height, nameStyle.top, vm.Get("g.screen_width").c_str(),
				vm.Get("g.screen_height").c_str(), vm.Get("g.msgalpha").c_str());
			if(ok)
				Pass(tally, "boot-macros-styles", detail);
			else
				Fail(tally, "boot-macros-styles",
					std::string(detail) + (booted ? "" : (" " + bootError)));
		}
		else
			Skip(tally, "boot-macros-styles", "no corpus: " + extracted);

		// 4. the first screen: real background, real line, game font
		if(!data.empty() && FileExists(scenePath) && DirExists(extracted))
		{
			const ScreenRun run = Play(options, true);
			bool ok = run.ok && run.captured && run.width == 1024 && run.height == 698 &&
				run.captureTextPixels > 500 && run.captureInkPixels > 500 &&
				run.captureColors > 1000 && run.captureBackgroundColors > 100 &&
				run.captureGlyphs > 5 && run.captureSpeaker == "少女" &&
				run.readBack && run.pngWidth == 1024 && run.pngHeight == 698 &&
				run.pngColors > 1000 && run.pngTextPixels > 500;
			char detail[512];
			snprintf(detail, sizeof detail,
				"canvas=%dx%d text=%zu ink=%zu colours=%zu bg=%zu glyphs=%d speaker=\"%s\" "
				"png=%dx%d/%zu colours/%zu text px",
				run.width, run.height, run.captureTextPixels, run.captureInkPixels,
				run.captureColors, run.captureBackgroundColors, run.captureGlyphs,
				run.captureSpeaker.c_str(), run.pngWidth, run.pngHeight, run.pngColors,
				run.pngTextPixels);
			if(ok)
				Pass(tally, "first-screen", detail);
			else
				Fail(tally, "first-screen",
					std::string(detail) + (run.error.empty() ? "" : (" | " + run.error)));
		}
		else
			Skip(tally, "first-screen", "no corpus: " + scenePath);

		// 5. the archive path and the unpacked path must agree byte for byte
		if(!data.empty() && FileExists(archivePath) && DirExists(extracted))
		{
			artemis::AssetSource fromArchive;
			artemis::AssetSource fromFiles;
			std::string error;
			const bool opened = fromArchive.AddArchive(archivePath, error);
			fromFiles.AddDirectory(extracted, error);
			const char* names[] = { "bgimage/main", "forsystem/msw2", "forsystem/name" };
			bool ok = opened;
			std::string detail;
			for(const char* name : names)
			{
				std::vector<uint8_t> a, b;
				std::string resolvedA, resolvedB, readError;
				const bool gotA = fromArchive.Read(name, a, resolvedA, readError);
				const bool gotB = fromFiles.Read(name, b, resolvedB, readError);
				if(!gotA || !gotB || a != b)
				{
					ok = false;
					detail += std::string(name) + " differs; ";
					continue;
				}
				detail += std::string(name) + "=" + std::to_string(a.size()) + "B ";
			}
			if(ok)
				Pass(tally, "archive-vs-files", detail);
			else
				Fail(tally, "archive-vs-files", detail + error);
		}
		else
			Skip(tally, "archive-vs-files", "needs root.pfs and extracted/");

		// 6. a JPEG background decodes (milkeyway is a JPEG, the rest is PNG)
		if(!data.empty() && DirExists(extracted))
		{
			artemis::AssetSource source;
			std::string error;
			source.AddDirectory(extracted, error);
			std::vector<uint8_t> bytes;
			std::string resolved, readError;
			artemis::RgbaImage image;
			const bool read = source.Read("bgimage/milkeyway", bytes, resolved, readError);
			const bool decoded = read && artemis::DecodeImage(bytes.data(), bytes.size(), image,
				readError);
			const size_t colors = decoded ? artemis::CountColors(image.rgba.data(),
				(size_t)image.width * image.height) : 0;
			const bool ok = decoded && image.width == 1024 && image.height == 768 &&
				colors > 1000;
			char detail[256];
			snprintf(detail, sizeof detail, "%s %dx%d %zu colours", resolved.c_str(),
				image.width, image.height, colors);
			if(ok)
				Pass(tally, "jpeg-background", detail);
			else
				Fail(tally, "jpeg-background",
					std::string(detail) + (readError.empty() ? "" : (" " + readError)));
		}
		else
			Skip(tally, "jpeg-background", "no corpus: " + extracted);

		// 7. texture cache: the same background is decoded once
		if(!data.empty() && DirExists(extracted))
		{
			artemis::AssetSource source;
			artemis::TextureCache cache;
			std::string error;
			source.AddDirectory(extracted, error);
			const artemis::RgbaImage* first = cache.Load(source, "bgimage/main", error);
			const artemis::RgbaImage* second = cache.Load(source, "bgimage/main", error);
			const bool ok = first != nullptr && second == first && cache.misses() == 1 &&
				cache.hits() == 1;
			char detail[256];
			snprintf(detail, sizeof detail, "hits=%zu misses=%zu bytes=%zu", cache.hits(),
				cache.misses(), cache.cachedBytes());
			if(ok)
				Pass(tally, "texture-cache", detail);
			else
				Fail(tally, "texture-cache", detail);
		}
		else
			Skip(tally, "texture-cache", "no corpus: " + extracted);

		// 8. the effect layer: six effects, each sampled at two times on a real
		// picture.  A missing corpus is a SKIP, like everywhere else here.
		RunFxCheck(tally, data);

		// 9. the shipped .ast corpus: every file parses, and every linknext chain
		// walks every block exactly once
		const std::string astCorpus = FindAstCorpus();
		if(!astCorpus.empty())
		{
			artemis::AstCorpusStats corpus;
			int defects = 0;
			std::string worst;
			for(const std::string& path : AstFiles(astCorpus))
			{
				AstFile file;
				std::string parseError;
				if(!LoadAstFile(path, file, parseError))
				{
					corpus.failures.push_back(path + ": " + parseError);
					corpus.failed++;
					continue;
				}
				const artemis::AstChainInfo chain = artemis::WalkAstChain(file.ast);
				artemis::AddToCorpus(corpus, file.ast, chain);
				if(!chain.ok)
				{
					defects++;
					if(worst.empty())
						worst = path + ": " + chain.error;
				}
			}
			const bool ok = corpus.files == 59 && corpus.failed == 0 && corpus.blocks == 13133 &&
				corpus.lines == 13073 && corpus.voices == 5693 && corpus.chainDefects == 0;
			char detail[512];
			snprintf(detail, sizeof detail,
				"files=%d failed=%d chained=%lld blocks=%lld commands=%lld lines=%lld "
				"voices=%lld chain_defects=%lld", corpus.files, corpus.failed, corpus.chained,
				corpus.blocks, corpus.commands, corpus.lines, corpus.voices,
				corpus.chainDefects);
			if(ok)
				Pass(tally, "ast-corpus-chain", detail);
			else
				Fail(tally, "ast-corpus-chain", std::string(detail) + " | " + worst);

			// one file in detail: the numbers a --ast-info run prints
			const std::string one = astCorpus + "/d01_c01_2.ast";
			AstFile file;
			std::string parseError;
			if(FileExists(one) && LoadAstFile(one, file, parseError))
			{
				const artemis::AstChainInfo chain = artemis::WalkAstChain(file.ast);
				const bool oneOk = chain.chained && file.ast.blocks.size() == 78 &&
					chain.withNext == 77 && chain.order.size() == 78 && chain.ok &&
					chain.repeated == 0 && chain.broken == 0 && chain.unreached.empty() &&
					chain.head == "block_00000";
				char oneDetail[512];
				snprintf(oneDetail, sizeof oneDetail,
					"d01_c01_2.ast: blocks=%zu with_linknext=%d walked=%zu head=%s "
					"repeated=%d broken=%d unreached=%zu ok=%d", file.ast.blocks.size(),
					chain.withNext, chain.order.size(), chain.head.c_str(), chain.repeated,
					chain.broken, chain.unreached.size(), (int)chain.ok);
				if(oneOk)
					Pass(tally, "ast-chain-one", oneDetail);
				else
					Fail(tally, "ast-chain-one", oneDetail);
			}
			else
				Skip(tally, "ast-chain-one", "no " + one);

			// and a real script drawn: the block whose `bg` the archive actually
			// ships (image/bg/pink.png), so the frame has a background from the game
			const std::string script = astCorpus + "/d09_hy01_3.ast";
			if(FileExists(script))
			{
				Options ast;
				ast.startBlock = "block_00061";
				ast.maxPages = 2;
				ast.frames = "/tmp/agame-ast-selftest";
				const AstPlayback run = PlayAst(script, ast);
				const bool ok = run.ok && run.pages == 2 && run.pagesWithText == 2 &&
					run.width == 1600 && run.height == 900 && run.stageOps >= 1 &&
					run.textPixels > 1000 && run.glyphs > 5 && run.chain.ok;
				char detail[512];
				snprintf(detail, sizeof detail,
					"%dx%d pages=%d inked=%d text=%zu px glyphs=%d layer_writes=%lld "
					"refused=%lld failures=%lld frames=%zu", run.width, run.height, run.pages,
					run.pagesWithText, run.textPixels, run.glyphs, run.stageOps,
					run.policySkipped, run.assetFailures, run.frames.size());
				if(ok)
					Pass(tally, "ast-render-real", detail);
				else
					Fail(tally, "ast-render-real",
						std::string(detail) + (run.error.empty() ? "" : (" | " + run.error)));
			}
			else
				Skip(tally, "ast-render-real", "no " + script);
		}
		else
		{
			Skip(tally, "ast-corpus-chain", "no corpus: testdata/artemis-henpri-trial");
			Skip(tally, "ast-chain-one", "no corpus");
			Skip(tally, "ast-render-real", "no corpus");
		}

		// 10. sound: the real `[bgm file="moon"]` out of the real scene script, mixed
		// offline into a WAV that can be checked with afinfo
		if(!data.empty() && FileExists(scenePath) && DirExists(extracted))
		{
			Options audioOptions = options;
			audioOptions.audioCapture = "/tmp/agame-selftest.wav";
			audioOptions.audioSeconds = 2.0;
			const ScreenRun run = Play(audioOptions, true);
			const bool ok = run.ok && run.audioCaptured && run.audioTracks >= 1 &&
				run.audioSeconds > 1.5 && run.audioRms > 0.01 && run.audioPeak > 0.05 &&
				!run.audioSilent;
			char detail[512];
			snprintf(detail, sizeof detail,
				"tracks=%zu capture=%s %.2fs rms=%.4f peak=%.3f silent=%d",
				run.audioTracks, run.audioPath.c_str(), run.audioSeconds, run.audioRms,
				run.audioPeak, (int)run.audioSilent);
			if(ok)
				Pass(tally, "audio-capture", detail);
			else
				Fail(tally, "audio-capture",
					std::string(detail) + (run.error.empty() ? "" : (" | " + run.error)));
		}
		else
			Skip(tally, "audio-capture", "no corpus");

		// 11. the input wiring the real scripts declare: VK code -> script label
		if(!data.empty() && FileExists(scenePath) && DirExists(extracted))
		{
			Options bindOptions = options;
			bindOptions.maxPages = 2;
			const ScreenRun run = Play(bindOptions, true);
			bool hasSave = false;
			for(const std::string& line : run.bindings)
				if(line.find("key=83") != std::string::npos && line.find("save") != std::string::npos)
					hasSave = true;
			const bool ok = run.ok && run.bindings.size() >= 8 && hasSave;
			char detail[512];
			snprintf(detail, sizeof detail,
				"setonpush declared=%zu taken=%zu; S(83) -> save present=%d",
				run.bindings.size(), run.bindingsExecuted, (int)hasSave);
			if(ok)
				Pass(tally, "input-bindings", detail);
			else
				Fail(tally, "input-bindings",
					std::string(detail) + (run.error.empty() ? "" : (" | " + run.error)));
		}
		else
			Skip(tally, "input-bindings", "no corpus");

		printf("\n%d passed, %d failed, %d skipped\n", tally.passed, tally.failed, tally.skipped);
		return tally.failed == 0 ? 0 : 1;
	}
}

// ===========================================================================
// main
// ===========================================================================

static void Usage(const char* program)
{
	fprintf(stderr,
		"usage:\n"
		"  %s <script.ast> [--frames <dir>] [--data-dir <extracted>] [--size WxH]\n"
		"     [--start-block block_NNNNN] [--max-pages N] [--font <file>]\n"
		"     [--audio-capture <file.wav>] [--audio-seconds N] [--show-text]\n"
		"     the tree's own .ast format plays as twelve frames; a *shipped* .ast is\n"
		"     recognised by content and played as a block chain (headless, frames to\n"
		"     --frames, nothing R18 leaves /tmp)\n"
		"  %s --ast-info <file.ast|directory> [--show-text]\n"
		"     parse and report only: no window, no framebuffer, no PNG\n"
		"  %s --script <scene.iet> --archive <root.pfs> [--data-dir <extracted>]\n"
		"     [--boot first.iet] [--label start] [--frames <dir>]\n"
		"     [--out <png>] [--max-pages N] [--verbose] [--fx] [--fx-at ms]\n"
		"     [--audio] [--audio-capture <file.wav>] [--audio-seconds N]\n"
		"  %s selftest [--data <dir>] [--archive <root.pfs>] [--verbose]\n",
		program, program, program, program);
}

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		Usage(argv[0]);
		return 2;
	}

	Options options;
	std::string astPath;
	std::string framesDir;
	bool ietMode = false;
	for(int i = 1; i < argc; i++)
	{
		const std::string arg = argv[i];
		const bool hasValue = i + 1 < argc;
		if(arg == "selftest")
			options.selftest = true;
		else if(arg == "--script" && hasValue)
		{
			options.script = argv[++i];
			ietMode = true;
		}
		else if(arg == "--archive" && hasValue)
		{
			options.archive = argv[++i];
			ietMode = true;
		}
		else if((arg == "--data-dir" || arg == "--data") && hasValue)
		{
			options.dataDir = argv[++i];
			ietMode = true;
		}
		else if(arg == "--boot" && hasValue)
		{
			options.boot = argv[++i];
			ietMode = true;
		}
		else if(arg == "--label" && hasValue)
			options.label = argv[++i];
		else if(arg == "--frames" && hasValue)
		{
			framesDir = argv[++i];
			options.framesGiven = true;
		}
		else if(arg == "--out" && hasValue)
		{
			options.out = argv[++i];
			options.outGiven = true;
		}
		else if((arg == "--max-pages" || arg == "--pages") && hasValue)
			options.maxPages = atoi(argv[++i]);
		else if(arg == "--frame-prefix" && hasValue)
			options.framePrefix = argv[++i];
		else if(arg == "--fade-frames" && hasValue)
			options.fadeFrames = atoi(argv[++i]);
		else if(arg == "--verbose" || arg == "-v")
			options.verbose = true;
		else if(arg == "--ast-info" && hasValue)
			options.astInfoPath = argv[++i];
		else if(arg == "--font" && hasValue)
			options.font = argv[++i];
		else if(arg == "--show-text")
			options.showText = true;
		else if(arg == "--max-blocks" && hasValue)
			options.maxBlocks = atoi(argv[++i]);
		else if(arg == "--start-block" && hasValue)
			options.startBlock = argv[++i];
		else if(arg == "--audio-capture" && hasValue)
			options.audioCapture = argv[++i];
		else if(arg == "--audio-seconds" && hasValue)
			options.audioSeconds = atof(argv[++i]);
		else if(arg == "--audio")
			options.audio = true;
		else if(arg == "--fx")
			options.fx = true;
		else if(arg == "--fx-at" && hasValue)
			options.fxAt = atof(argv[++i]);
		else if(arg == "--play")
			options.play = true;
		else if(arg == "--no-menu")
			options.startAtMenu = false;
		else if(arg == "--real-time")
			options.realTime = true;
		else if(arg == "--no-vsync")
			options.vsync = false;
		else if(arg == "--hash-frames")
			options.hashFrames = true;
		else if((arg == "--autoplay-keys" || arg == "--keys") && hasValue)
		{
			std::string list = argv[++i];
			size_t at = 0;
			while(at <= list.size())
			{
				const size_t comma = list.find(',', at);
				const std::string part = list.substr(at,
					comma == std::string::npos ? std::string::npos : comma - at);
				if(!part.empty())
					options.autoplayKeys.push_back(part);
				if(comma == std::string::npos)
					break;
				at = comma + 1;
			}
			options.play = true;
		}
		else if(arg == "--shot-title" && hasValue)
		{
			options.shotTitle = argv[++i];
			options.play = true;
		}
		else if(arg == "--shot-scene" && hasValue)
		{
			options.shotScene = argv[++i];
			options.play = true;
		}
		else if(arg == "--shot-dir" && hasValue)
		{
			options.shotDir = argv[++i];
			options.play = true;
		}
		else if(arg == "--play-pages" && hasValue)
			options.quitAfterPages = atoi(argv[++i]);
		else if(arg == "--size" && hasValue)
		{
			options.size = argv[++i];
			const size_t x = options.size.find('x');
			if(x != std::string::npos)
			{
				options.width = atoi(options.size.substr(0, x).c_str());
				options.height = atoi(options.size.substr(x + 1).c_str());
				options.haveSize = options.width > 0 && options.height > 0;
			}
			if(!options.haveSize)
			{
				fprintf(stderr, "agame: --size wants WxH, not %s\n", options.size.c_str());
				return 2;
			}
		}
		else if(!arg.empty() && arg[0] != '-')
			astPath = arg;
	}
	options.frames = framesDir;

	if(options.selftest)
		return RunSelfTest(options);

	if(!options.astInfoPath.empty())
		return RunAstInfo(options);

	// The interactive window.  `--play` asks for it; a bare
	// `agame --script … --data-dir …` asks for it too, but only on a terminal:
	// a piped or CI run must not open a window (not even a dummy one) by
	// accident, so it keeps the still-frame behaviour and gets told how to ask.
	{
		const bool ietWanted = ietMode || (!astPath.empty() && astPath.size() > 4 &&
			astPath.compare(astPath.size() - 4, 4, ".iet") == 0);
		bool wantPlay = options.play;
		if(!wantPlay && ietWanted && !options.framesGiven && !options.outGiven)
		{
			if(isatty(STDOUT_FILENO) != 0)
				wantPlay = true;
			else
				fprintf(stderr, "agame: no --frames/--out and stdout is not a terminal: "
					"pass --play to open the window, or --frames <dir> for a still run\n");
		}
		if(wantPlay)
		{
			if(!ietWanted)
			{
				fprintf(stderr, "agame: --play needs --script <scene.iet> and data "
					"(--data-dir/--archive): a shipped .ast title has no first.iet menu\n");
				return 2;
			}
			const PlayRun run = RunInteractive(options);
			if(!run.ok)
			{
				fprintf(stderr, "agame: %s\n", run.error.c_str());
				return 1;
			}
			PrintPlayReport(run);
			const bool good = run.pages > 0 && run.stats.frames > 0;
			printf("verdict: %s\n", good ? "the game ran and advanced by clicks"
				: "nothing was advanced");
			return good ? 0 : 1;
		}
	}

	if(ietMode || (!astPath.empty() && astPath.size() > 4 &&
		astPath.compare(astPath.size() - 4, 4, ".iet") == 0))
	{
		if(options.script.empty())
		{
			fprintf(stderr, "agame: --script is required for the .iet path\n");
			return 2;
		}
		const ScreenRun run = Play(options, false);
		if(!run.ok)
		{
			fprintf(stderr, "agame: %s\n", run.error.c_str());
			return 1;
		}
		artemis::AssetSource assets;
		std::string error;
		if(!options.archive.empty())
			assets.AddArchive(options.archive, error);
		if(!options.dataDir.empty())
			assets.AddDirectory(options.dataDir, error);
		PrintReport(run, assets.Describe());
		if(!run.captured)
		{
			fprintf(stderr, "agame: no page had both a real background and text\n");
			return 1;
		}
		// the same assertions the self test makes, so a hand run cannot pass a
		// frame the test would reject
		const bool good = run.width == 1024 && run.height == 698 &&
			run.captureTextPixels > 500 && run.captureBackgroundColors > 100 && run.readBack &&
			run.pngColors > 1000 && run.pngTextPixels > 500;
		printf("verdict: %s\n", good ? "first screen verified" : "first screen NOT verified");
		return good ? 0 : 1;
	}

	if(astPath.empty())
	{
		Usage(argv[0]);
		return 2;
	}

	// Which reader gets the file is decided by its *content*: ParseLuaAst is the
	// authority on shipped Lua scripts, and when it refuses the file the old
	// reader takes over, which is what keeps the twelve-frame demonstration
	// (tests/sample-scene.ast, the tree's own invented format) exactly as it was.
	{
		const std::string source = ReadFile(astPath.c_str());
		artemis::LuaAst ast;
		std::string parseError;
		if(!source.empty() && artemis::ParseLuaAst(source, ast, parseError))
		{
			const AstPlayback run = PlayAst(astPath, options);
			if(!run.ok)
			{
				fprintf(stderr, "agame: %s\n", run.error.c_str());
				return 1;
			}
			PrintAstReport(run);
			const bool good = run.pages > 0 && run.width > 0 && run.pagesWithText > 0 &&
				run.textPixels > 100 && run.glyphs > 0;
			printf("verdict: %s\n",
				good ? "a shipped .ast played and put words on the stage"
					: "the chain ran but no page put words on the stage");
			return good ? 0 : 1;
		}
		if(options.verbose && !source.empty())
			fprintf(stderr, "agame: not a shipped Lua .ast (%s); the demo reader takes it\n",
				parseError.c_str());
	}
	return RunAst(astPath, framesDir, options.out);
}
