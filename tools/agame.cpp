// agame - play a scenario on a window
//
// The runtime's actions become a picture: a background, a message box and the
// current line, drawn with the system's CoreText so Japanese comes out right.
// Space or a click advances; with SDL_VIDEODRIVER=dummy the player advances on
// its own and can write frames out, which is how this is checked without a game.
//
//   agame <script.ast>                    play it
//   agame <script.ast> --frames <dir>     headless: write one picture per line
//
// Everything above the drawing is the engine's own: the scenario runtime decides
// what to show and when.

#include "../src/runtime.h"

#include <SDL.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

// ---------------------------------------------------------------------------
// text
// ---------------------------------------------------------------------------
//
// One line at a time through CoreText: a font of a size that suits a message box,
// drawn white on a dark box.  Returns the pixels as BGRA, which is what SDL wants.

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

// ---------------------------------------------------------------------------
// the window
// ---------------------------------------------------------------------------

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

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		fprintf(stderr, "usage: %s <script.ast> [--frames <dir>]\n", argv[0]);
		return 2;
	}
	std::string framesDir;
	for(int i = 2; i + 1 < argc; i++)
		if(strcmp(argv[i], "--frames") == 0)
			framesDir = argv[i + 1];
	const bool headless = !framesDir.empty();

	std::string source = ReadFile(argv[1]);
	if(source.empty())
	{
		fprintf(stderr, "agame: cannot read %s\n", argv[1]);
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
					screen.Save(path);
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
	return 0;
}
