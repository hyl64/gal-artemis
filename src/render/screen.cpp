// screen.cpp - assets, the texture cache, the canvas, CoreText, the stage
//
// The order of business on the way to a frame:
//
//   script name ("bgimage/milkeyway")
//     -> AssetSource: normalise, try the extensions the release ships, ask the
//        archive first (its index is mmapped, only the member's bytes are read)
//        then an unpacked tree
//     -> DecodeImage: PNG through pngio, JPEG through ImageIO
//     -> TextureCache: keyed on the concrete member, so a second frame with the
//        same background is a map lookup
//     -> Stage::Composite: layers in id order, then the words
//
// The words are the game's own TTF, loaded from the game's own bytes through
// CGFont/CTFont, laid out with CTFramesetter inside the box the game's first.iet
// asked for, with the game's shadow colour under them.

#include "screen.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <ImageIO/ImageIO.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <unordered_set>

namespace artemis
{
	// ---------------------------------------------------------------------------
	// colours
	// ---------------------------------------------------------------------------
	bool ParseColor(const std::string& raw, uint8_t rgba[4])
	{
		std::string text = raw;
		if(text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0)
			text = text.substr(2);
		if(!text.empty() && text[0] == '#')
			text = text.substr(1);
		if(text.size() != 6 && text.size() != 8)
			return false;
		for(char c : text)
			if(!std::isxdigit((unsigned char)c))
				return false;
		const unsigned long value = strtoul(text.c_str(), nullptr, 16);
		if(text.size() == 8)
		{
			rgba[0] = (uint8_t)((value >> 16) & 0xff);
			rgba[1] = (uint8_t)((value >> 8) & 0xff);
			rgba[2] = (uint8_t)(value & 0xff);
			rgba[3] = (uint8_t)((value >> 24) & 0xff);
		}
		else
		{
			rgba[0] = (uint8_t)((value >> 16) & 0xff);
			rgba[1] = (uint8_t)((value >> 8) & 0xff);
			rgba[2] = (uint8_t)(value & 0xff);
			rgba[3] = 255;
		}
		return true;
	}

	// ---------------------------------------------------------------------------
	// assets
	// ---------------------------------------------------------------------------
	const std::vector<std::string>& AssetSource::Extensions()
	{
		// scripts are named without their extension in the real data
		// ("system/develop_tool", "scenario/scene"), images as often as not too
		// ("bgimage/milkeyway" is a .jpg, "bgimage/main" a .png)
		static const std::vector<std::string> list = {
			"", ".png", ".jpg", ".jpeg", ".bmp", ".webp", ".iet", ".PNG", ".JPG"
		};
		return list;
	}

	namespace
	{
		std::string Normalize(const std::string& name)
		{
			std::string out;
			out.reserve(name.size());
			for(char c : name)
				out += (c == '\\') ? '/' : c;
			while(out.rfind("./", 0) == 0)
				out = out.substr(2);
			while(!out.empty() && out.front() == '/')
				out.erase(out.begin());
			return out;
		}

		std::string Lower(std::string text)
		{
			for(char& c : text)
				c = (char)std::tolower((unsigned char)c);
			return text;
		}

		uint64_t Fnv(const uint8_t* data, size_t size)
		{
			uint64_t hash = 1469598103934665603ull;
			for(size_t i = 0; i < size; i++)
			{
				hash ^= data[i];
				hash *= 1099511628211ull;
			}
			return hash;
		}
	}

	bool AssetSource::AddArchive(const std::string& path, std::string& error)
	{
		try
		{
			ArchiveSlot slot;
			slot.path = path;
			slot.archive = Open(path);
			slot.magic = slot.archive.magic;
			slot.members = slot.archive.entries.size();
			m_archives.push_back(std::move(slot));
			return true;
		}
		catch(const std::exception& failure)
		{
			error = std::string("cannot open archive ") + path + ": " + failure.what();
			return false;
		}
	}

	bool AssetSource::AddDirectory(const std::string& path, std::string& error)
	{
		std::error_code code;
		if(!std::filesystem::is_directory(path, code))
		{
			error = "not a directory: " + path;
			return false;
		}
		DirectorySlot slot;
		slot.path = path;
		m_directories.push_back(std::move(slot));
		return true;
	}

	size_t AssetSource::memberCount() const
	{
		size_t count = 0;
		for(const ArchiveSlot& slot : m_archives)
			count += slot.members;
		return count;
	}

	std::string AssetSource::Describe() const
	{
		std::string out;
		for(const ArchiveSlot& slot : m_archives)
		{
			if(!out.empty())
				out += " + ";
			out += slot.path + " (" + slot.magic + ", " + std::to_string(slot.members) +
				" members)";
		}
		for(const DirectorySlot& slot : m_directories)
		{
			if(!out.empty())
				out += " + ";
			out += slot.path + "/";
		}
		return out;
	}

	bool AssetSource::ReadArchive(const ArchiveSlot& slot, const std::string& logical,
		std::vector<uint8_t>& out, std::string& resolved) const
	{
		const std::string normalized = Normalize(logical);
		for(const std::string& ext : Extensions())
		{
			const std::string candidate = normalized + ext;
			const Entry* entry = Find(slot.archive, candidate);
			if(entry == nullptr)
				continue;
			try
			{
				out = Content(slot.archive, *entry);
			}
			catch(const std::exception& failure)
			{
				(void)failure;
				continue;
			}
			resolved = slot.path + "!" + NormalizeName(entry->name);
			return true;
		}
		return false;
	}

	bool AssetSource::ReadDirectory(const DirectorySlot& slot, const std::string& logical,
		std::vector<uint8_t>& out, std::string& resolved) const
	{
		if(!slot.indexed)
		{
			std::error_code code;
			for(std::filesystem::recursive_directory_iterator it(slot.path, code), end;
				it != end && !code; it.increment(code))
			{
				if(!it->is_regular_file())
					continue;
				std::string relative = std::filesystem::relative(it->path(), slot.path, code).string();
				if(code)
					break;
				slot.index[Lower(Normalize(relative))] = Normalize(relative);
			}
			slot.indexed = true;
		}
		const std::string normalized = Normalize(logical);
		for(const std::string& ext : Extensions())
		{
			const std::string candidate = Lower(normalized + ext);
			std::map<std::string, std::string>::const_iterator found = slot.index.find(candidate);
			if(found == slot.index.end())
				continue;
			const std::string full = slot.path + "/" + found->second;
			FILE* file = fopen(full.c_str(), "rb");
			if(file == nullptr)
				continue;
			fseek(file, 0, SEEK_END);
			const long size = ftell(file);
			fseek(file, 0, SEEK_SET);
			out.clear();
			if(size > 0)
			{
				out.resize((size_t)size);
				if(fread(out.data(), 1, out.size(), file) != out.size())
					out.clear();
			}
			fclose(file);
			if(out.empty())
				continue;
			resolved = full;
			return true;
		}
		return false;
	}

	bool AssetSource::Read(const std::string& logical, std::vector<uint8_t>& out,
		std::string& resolved, std::string& error) const
	{
		for(const ArchiveSlot& slot : m_archives)
			if(ReadArchive(slot, logical, out, resolved))
				return true;
		for(const DirectorySlot& slot : m_directories)
			if(ReadDirectory(slot, logical, out, resolved))
				return true;
		error = "asset not found: " + logical;
		return false;
	}

	bool AssetSource::Exists(const std::string& logical) const
	{
		std::vector<uint8_t> bytes;
		std::string resolved, error;
		return Read(logical, bytes, resolved, error);
	}

	// ---------------------------------------------------------------------------
	// decode
	// ---------------------------------------------------------------------------
	namespace
	{
		bool DecodeWithImageIO(const uint8_t* data, size_t size, RgbaImage& out,
			std::string& error)
		{
			CGDataProviderRef provider = CGDataProviderCreateWithData(nullptr, data, size, nullptr);
			if(provider == nullptr)
			{
				error = "ImageIO: cannot wrap the bytes";
				return false;
			}
			CGImageSourceRef source = CGImageSourceCreateWithDataProvider(provider, nullptr);
			CGDataProviderRelease(provider);
			if(source == nullptr)
			{
				error = "ImageIO: not a recognised image";
				return false;
			}
			CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
			CFRelease(source);
			if(image == nullptr)
			{
				error = "ImageIO: cannot decode the image";
				return false;
			}
			const size_t width = CGImageGetWidth(image);
			const size_t height = CGImageGetHeight(image);
			if(width == 0 || height == 0 || width > 32768 || height > 32768)
			{
				CGImageRelease(image);
				error = "ImageIO: implausible image size";
				return false;
			}
			std::vector<uint8_t> buffer(width * height * 4, 0);
			CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
			CGContextRef context = CGBitmapContextCreate(buffer.data(), width, height, 8,
				width * 4, space,
				kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
			CGColorSpaceRelease(space);
			if(context == nullptr)
			{
				CGImageRelease(image);
				error = "ImageIO: cannot make a bitmap context";
				return false;
			}
			CGContextSetBlendMode(context, kCGBlendModeCopy);
			CGContextDrawImage(context, CGRectMake(0, 0, (CGFloat)width, (CGFloat)height), image);
			CGContextRelease(context);
			CGImageRelease(image);

			// ImageIO hands back premultiplied bytes; the compositor blends straight
			// alpha, so undo it.  A JPEG is opaque and this is a no-op.
			bool anyAlpha = false;
			for(size_t i = 3; i < buffer.size(); i += 4)
				if(buffer[i] != 255)
				{
					anyAlpha = true;
					break;
				}
			if(anyAlpha)
			{
				for(size_t i = 0; i < buffer.size(); i += 4)
				{
					const unsigned a = buffer[i + 3];
					if(a == 0 || a == 255)
						continue;
					for(int k = 0; k < 3; k++)
					{
						const unsigned value = (buffer[i + k] * 255u + a / 2) / a;
						buffer[i + k] = (uint8_t)std::min(255u, value);
					}
				}
			}
			out.width = (int)width;
			out.height = (int)height;
			out.rgba = std::move(buffer);
			return true;
		}
	}

	bool DecodeImage(const uint8_t* data, size_t size, RgbaImage& out, std::string& error)
	{
		out = RgbaImage();
		if(data == nullptr || size < 8)
		{
			error = "empty image";
			return false;
		}
		static const uint8_t pngMagic[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
		if(memcmp(data, pngMagic, 8) == 0)
		{
			Image decoded;
			if(!ReadPng(data, size, decoded, error))
				return false;
			out.width = decoded.width;
			out.height = decoded.height;
			out.rgba = std::move(decoded.rgba);
			return true;
		}
		if(data[0] == 0xff && data[1] == 0xd8)
			return DecodeWithImageIO(data, size, out, error);
		if(memcmp(data, "RIFF", 4) == 0 && size > 12 && memcmp(data + 8, "WEBP", 4) == 0)
			return DecodeWithImageIO(data, size, out, error);
		error = "unrecognised image format (not PNG or JPEG)";
		return false;
	}

	// ---------------------------------------------------------------------------
	// texture cache
	// ---------------------------------------------------------------------------
	const RgbaImage* TextureCache::Load(const AssetSource& source, const std::string& logical,
		std::string& error, std::string* resolvedOut)
	{
		// the key is the concrete member, so "bgimage/main" and "bgimage/main.png"
		// share one decode
		const std::chrono::steady_clock::time_point readStart =
			std::chrono::steady_clock::now();
		std::vector<uint8_t> bytes;
		std::string resolved, readError;
		if(!source.Read(logical, bytes, resolved, readError))
		{
			error = readError;
			return nullptr;
		}
		m_readMs += std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - readStart).count();
		if(resolvedOut != nullptr)
			*resolvedOut = resolved;
		std::map<std::string, Entry>::iterator found = m_entries.find(resolved);
		if(found != m_entries.end())
		{
			m_hits++;
			return &found->second.image;
		}
		Entry entry;
		entry.resolved = resolved;
		const std::chrono::steady_clock::time_point decodeStart =
			std::chrono::steady_clock::now();
		if(!DecodeImage(bytes.data(), bytes.size(), entry.image, error))
		{
			error = resolved + ": " + error;
			return nullptr;
		}
		m_decodeMs += std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - decodeStart).count();
		m_misses++;
		std::pair<std::map<std::string, Entry>::iterator, bool> inserted =
			m_entries.insert(std::make_pair(resolved, std::move(entry)));
		return &inserted.first->second.image;
	}

	size_t TextureCache::cachedBytes() const
	{
		size_t total = 0;
		for(const std::pair<const std::string, Entry>& item : m_entries)
			total += item.second.image.bytes();
		return total;
	}

	std::vector<std::string> TextureCache::names() const
	{
		std::vector<std::string> out;
		for(const std::pair<const std::string, Entry>& item : m_entries)
			out.push_back(item.first);
		return out;
	}

	// ---------------------------------------------------------------------------
	// fonts
	// ---------------------------------------------------------------------------
	struct FontLibrary::Impl
	{
		struct Face
		{
			CGFontRef font = nullptr;
			std::string postscript;
			std::string sha1;
			size_t bytes = 0;
		};
		std::map<std::string, Face> faces;

		~Impl()
		{
			for(std::pair<const std::string, Face>& item : faces)
				if(item.second.font != nullptr)
					CGFontRelease(item.second.font);
		}
	};

	namespace
	{
		void FreeBytes(void*, const void* data, size_t)
		{
			delete[] (const uint8_t*)data;
		}

		std::string Hex(const uint8_t* data, size_t size)
		{
			static const char* digits = "0123456789abcdef";
			std::string out;
			out.reserve(size * 2);
			for(size_t i = 0; i < size; i++)
			{
				out += digits[data[i] >> 4];
				out += digits[data[i] & 0xf];
			}
			return out;
		}

		CFStringRef CreateString(const std::string& utf8)
		{
			return CFStringCreateWithBytes(kCFAllocatorDefault, (const UInt8*)utf8.data(),
				(CFIndex)utf8.size(), kCFStringEncodingUTF8, false);
		}

		CFAttributedStringRef MakeAttributed(const std::string& utf8, CTFontRef font,
			CGColorRef colour, CTParagraphStyleRef paragraph)
		{
			CFStringRef text = CreateString(utf8);
			if(text == nullptr)
				return nullptr;
			const void* keys[] = { kCTFontAttributeName, kCTForegroundColorAttributeName,
				kCTParagraphStyleAttributeName };
			const void* values[] = { font, colour, paragraph };
			CFDictionaryRef attributes = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 3,
				&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			CFAttributedStringRef attributed =
				CFAttributedStringCreate(kCFAllocatorDefault, text, attributes);
			CFRelease(attributes);
			CFRelease(text);
			return attributed;
		}

		size_t CountGlyphs(CTFrameRef frame)
		{
			size_t glyphs = 0;
			CFArrayRef lines = CTFrameGetLines(frame);
			const CFIndex count = CFArrayGetCount(lines);
			for(CFIndex i = 0; i < count; i++)
			{
				CTLineRef line = (CTLineRef)CFArrayGetValueAtIndex(lines, i);
				glyphs += (size_t)CTLineGetGlyphCount(line);
			}
			return glyphs;
		}
	}

	FontLibrary::FontLibrary() : m_impl(new Impl()) {}

	FontLibrary::~FontLibrary() = default;

	bool FontLibrary::Load(const std::string& face, const std::vector<uint8_t>& data,
		std::string& error)
	{
		if(data.size() < 12)
		{
			error = face + ": too small to be a font";
			return false;
		}
		const uint8_t* head = data.data();
		const bool sfnt = (head[0] == 0x00 && head[1] == 0x01 && head[2] == 0x00 &&
			head[3] == 0x00) ||
			(memcmp(head, "OTTO", 4) == 0) || (memcmp(head, "true", 4) == 0) ||
			(memcmp(head, "ttcf", 4) == 0);
		if(!sfnt)
		{
			error = face + ": not an sfnt/TrueType font";
			return false;
		}
		uint8_t* copy = new uint8_t[data.size()];
		memcpy(copy, data.data(), data.size());
		CGDataProviderRef provider = CGDataProviderCreateWithData(nullptr, copy, data.size(),
			FreeBytes);
		if(provider == nullptr)
		{
			delete[] copy;
			error = face + ": cannot wrap the font bytes";
			return false;
		}
		CGFontRef font = CGFontCreateWithDataProvider(provider);
		CGDataProviderRelease(provider);
		if(font == nullptr)
		{
			error = face + ": CoreGraphics rejected the font";
			return false;
		}
		// register so CoreText can also fall back to it for missing glyphs
		CFErrorRef registerError = nullptr;
		if(!CTFontManagerRegisterGraphicsFont(font, &registerError))
		{
			if(registerError != nullptr)
				CFRelease(registerError);
		}
		CTFontRef probe = CTFontCreateWithGraphicsFont(font, 20.0, nullptr, nullptr);
		Impl::Face entry;
		entry.font = font;
		entry.bytes = data.size();
		if(probe != nullptr)
		{
			CFStringRef name = CTFontCopyPostScriptName(probe);
			if(name != nullptr)
			{
				char buffer[256] = { 0 };
				CFStringGetCString(name, buffer, sizeof buffer, kCFStringEncodingUTF8);
				entry.postscript = buffer;
				CFRelease(name);
			}
			CFRelease(probe);
		}
		uint8_t digest[20];
		Sha1(data.data(), data.size(), digest);
		entry.sha1 = Hex(digest, sizeof digest);
		m_impl->faces[face] = entry;
		return true;
	}

	bool FontLibrary::Has(const std::string& face) const
	{
		return m_impl->faces.find(face) != m_impl->faces.end();
	}

	std::vector<std::string> FontLibrary::faces() const
	{
		std::vector<std::string> out;
		for(const std::pair<const std::string, Impl::Face>& item : m_impl->faces)
			out.push_back(item.first);
		return out;
	}

	std::string FontLibrary::PostScriptName(const std::string& face) const
	{
		std::map<std::string, Impl::Face>::const_iterator found = m_impl->faces.find(face);
		return found == m_impl->faces.end() ? std::string() : found->second.postscript;
	}

	std::string FontLibrary::Sha1Hex(const std::string& face) const
	{
		std::map<std::string, Impl::Face>::const_iterator found = m_impl->faces.find(face);
		return found == m_impl->faces.end() ? std::string() : found->second.sha1;
	}

	size_t FontLibrary::byteSize(const std::string& face) const
	{
		std::map<std::string, Impl::Face>::const_iterator found = m_impl->faces.find(face);
		return found == m_impl->faces.end() ? 0 : found->second.bytes;
	}

	bool FontLibrary::Render(const TextStyle& style, const std::string& utf8, RgbaImage& out,
		int& glyphs, std::string& error)
	{
		glyphs = 0;
		out = RgbaImage();
		std::map<std::string, Impl::Face>::const_iterator found = m_impl->faces.find(style.face);
		if(found == m_impl->faces.end())
		{
			error = "font not loaded: " + style.face;
			return false;
		}
		if(style.width <= 0 || style.height <= 0)
		{
			error = "text box is empty";
			return false;
		}
		CTFontRef font = CTFontCreateWithGraphicsFont(found->second.font, (CGFloat)style.size,
			nullptr, nullptr);
		if(font == nullptr)
		{
			error = "CoreText cannot make a font of " + style.face;
			return false;
		}

		CTTextAlignment alignment = kCTLeftTextAlignment;
		if(style.align == 1)
			alignment = kCTCenterTextAlignment;
		else if(style.align == 2)
			alignment = kCTRightTextAlignment;
		CGFloat spacing = (CGFloat)style.lineSpacing;
		const CTParagraphStyleSetting settings[] = {
			{ kCTParagraphStyleSpecifierAlignment, sizeof alignment, &alignment },
			{ kCTParagraphStyleSpecifierLineSpacingAdjustment, sizeof spacing, &spacing },
		};
		CTParagraphStyleRef paragraph = CTParagraphStyleCreate(settings,
			sizeof settings / sizeof settings[0]);

		CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
		const CGFloat white[] = { style.color[0] / 255.0, style.color[1] / 255.0,
			style.color[2] / 255.0, 1.0 };
		const CGFloat shade[] = { style.shadowColor[0] / 255.0, style.shadowColor[1] / 255.0,
			style.shadowColor[2] / 255.0, 1.0 };
		CGColorRef ink = CGColorCreate(space, white);
		CGColorRef shadowInk = CGColorCreate(space, shade);
		CGColorSpaceRelease(space);

		std::vector<uint8_t> buffer((size_t)style.width * style.height * 4, 0);
		CGColorSpaceRef bufferSpace = CGColorSpaceCreateDeviceRGB();
		CGContextRef context = CGBitmapContextCreate(buffer.data(), style.width, style.height, 8,
			(size_t)style.width * 4, bufferSpace,
			kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
		CGColorSpaceRelease(bufferSpace);
		if(context == nullptr)
		{
			CGColorRelease(ink);
			CGColorRelease(shadowInk);
			CFRelease(paragraph);
			CFRelease(font);
			error = "cannot make a text bitmap";
			return false;
		}
		// No CTM flip: a frame lays its first line out at the top of the path rect,
		// and the bitmap's first memory row is the top row, so the picture and the
		// glyphs already agree.  Flipping here drew the text upside down.
		CGContextSetTextMatrix(context, CGAffineTransformIdentity);

		CGPathRef path = CGPathCreateWithRect(
			CGRectMake(0, 0, style.width, style.height), nullptr);
		size_t totalGlyphs = 0;
		for(int pass = 0; pass < (style.shadow ? 2 : 1); pass++)
		{
			const bool shadowPass = style.shadow && pass == 0;
			CFAttributedStringRef attributed = MakeAttributed(utf8, font,
				shadowPass ? shadowInk : ink, paragraph);
			if(attributed == nullptr)
				continue;
			CTFramesetterRef setter = CTFramesetterCreateWithAttributedString(attributed);
			CTFrameRef frame = CTFramesetterCreateFrame(setter, CFRangeMake(0, 0), path, nullptr);
			if(frame != nullptr)
			{
				CGContextSaveGState(context);
				if(shadowPass)
					CGContextTranslateCTM(context, 1.5, -1.5);
				CTFrameDraw(frame, context);
				CGContextRestoreGState(context);
				if(!shadowPass)
					totalGlyphs = CountGlyphs(frame);
				CFRelease(frame);
			}
			CFRelease(setter);
			CFRelease(attributed);
		}
		CGPathRelease(path);
		CGContextRelease(context);
		CGColorRelease(ink);
		CGColorRelease(shadowInk);
		CFRelease(paragraph);
		CFRelease(font);

		// straighten the premultiplied bytes into what the canvas blends
		for(size_t i = 0; i < buffer.size(); i += 4)
		{
			const unsigned alpha = buffer[i + 3];
			if(alpha == 0)
			{
				buffer[i] = buffer[i + 1] = buffer[i + 2] = 0;
				continue;
			}
			if(alpha == 255)
				continue;
			for(int k = 0; k < 3; k++)
				buffer[i + k] = (uint8_t)std::min(255u, (buffer[i + k] * 255u + alpha / 2) / alpha);
		}
		out.width = style.width;
		out.height = style.height;
		out.rgba = std::move(buffer);
		glyphs = (int)totalGlyphs;
		return true;
	}

	// ---------------------------------------------------------------------------
	// canvas
	// ---------------------------------------------------------------------------
	Canvas::Canvas(int width, int height)
		: m_width(width), m_height(height), m_pixels((size_t)width * height * 4, 0)
	{
	}

	void Canvas::Clear(uint8_t r, uint8_t g, uint8_t b)
	{
		for(size_t i = 0; i < m_pixels.size(); i += 4)
		{
			m_pixels[i] = r;
			m_pixels[i + 1] = g;
			m_pixels[i + 2] = b;
			m_pixels[i + 3] = 255;
		}
	}

	void Canvas::Fill(int x, int y, int w, int h, const uint8_t rgba[4])
	{
		const int left = std::max(0, x), top = std::max(0, y);
		const int right = std::min(m_width, x + w), bottom = std::min(m_height, y + h);
		for(int yy = top; yy < bottom; yy++)
			for(int xx = left; xx < right; xx++)
			{
				uint8_t* dst = &m_pixels[((size_t)yy * m_width + xx) * 4];
				const unsigned alpha = rgba[3];
				if(alpha == 255)
				{
					dst[0] = rgba[0];
					dst[1] = rgba[1];
					dst[2] = rgba[2];
					dst[3] = 255;
					continue;
				}
				if(alpha == 0)
					continue;
				for(int k = 0; k < 3; k++)
					dst[k] = (uint8_t)((rgba[k] * alpha + dst[k] * (255 - alpha)) / 255);
			}
	}

	void Canvas::Blend(const uint8_t* src, int srcWidth, int srcHeight, int left, int top,
		int alpha)
	{
		if(src == nullptr || srcWidth <= 0 || srcHeight <= 0 || alpha <= 0)
			return;
		const int x0 = std::max(0, left), y0 = std::max(0, top);
		const int x1 = std::min(m_width, left + srcWidth);
		const int y1 = std::min(m_height, top + srcHeight);
		for(int y = y0; y < y1; y++)
		{
			const size_t srcRow = (size_t)(y - top) * srcWidth;
			for(int x = x0; x < x1; x++)
			{
				const uint8_t* s = &src[(srcRow + (size_t)(x - left)) * 4];
				unsigned sa = s[3];
				if(sa == 0)
					continue;
				if(alpha != 255)
					sa = sa * (unsigned)alpha / 255u;
				if(sa == 0)
					continue;
				uint8_t* d = &m_pixels[((size_t)y * m_width + x) * 4];
				if(sa == 255)
				{
					d[0] = s[0];
					d[1] = s[1];
					d[2] = s[2];
					d[3] = 255;
					continue;
				}
				for(int k = 0; k < 3; k++)
					d[k] = (uint8_t)((s[k] * sa + d[k] * (255 - sa)) / 255);
				d[3] = 255;
			}
		}
	}

	uint32_t Canvas::At(int x, int y) const
	{
		if(x < 0 || y < 0 || x >= m_width || y >= m_height)
			return 0;
		const uint8_t* p = &m_pixels[((size_t)y * m_width + x) * 4];
		return ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
	}

	size_t Canvas::distinctColors() const
	{
		std::unordered_set<uint32_t> seen;
		seen.reserve((size_t)m_width * m_height / 4 + 16);
		for(size_t i = 0; i < m_pixels.size(); i += 4)
			seen.insert(((uint32_t)m_pixels[i] << 16) | ((uint32_t)m_pixels[i + 1] << 8) |
				m_pixels[i + 2]);
		return seen.size();
	}

	size_t Canvas::differingPixels(const Canvas& other, int x, int y, int w, int h) const
	{
		size_t count = 0;
		const int x0 = std::max(0, x), y0 = std::max(0, y);
		const int x1 = std::min(std::min(m_width, other.m_width), x + w);
		const int y1 = std::min(std::min(m_height, other.m_height), y + h);
		for(int yy = y0; yy < y1; yy++)
			for(int xx = x0; xx < x1; xx++)
			{
				const size_t at = ((size_t)yy * m_width + xx) * 4;
				if(memcmp(&m_pixels[at], &other.m_pixels[at], 3) != 0)
					count++;
			}
		return count;
	}

	// ---------------------------------------------------------------------------
	// stage
	// ---------------------------------------------------------------------------
	namespace
	{
		// a layer set ("86") moves its children ("86.0.0") with it
		void AncestorOffsets(const std::map<std::string, Layer>& layers, const std::string& id,
			int& left, int& top)
		{
			left = 0;
			top = 0;
			std::string current = id;
			for(;;)
			{
				const size_t dot = current.rfind('.');
				if(dot == std::string::npos)
					break;
				current = current.substr(0, dot);
				std::map<std::string, Layer>::const_iterator found = layers.find(current);
				if(found != layers.end())
				{
					left += found->second.left;
					top += found->second.top;
				}
			}
		}

		void SplitLayerId(const std::string& id, int& major, int& sub)
		{
			size_t dot = id.find('.');
			const std::string head = dot == std::string::npos ? id : id.substr(0, dot);
			const std::string tail = dot == std::string::npos ? std::string() : id.substr(dot + 1);
			major = head.empty() ? 0 : atoi(head.c_str());
			sub = tail.empty() ? 0 : atoi(tail.c_str());
			if(head.empty() || !std::isdigit((unsigned char)head[0]))
				major = 1000; // a named layer sorts after the numbered ones
		}
	}

	Stage::Stage(int width, int height) : m_width(width), m_height(height) {}

	bool Stage::SetLayerImage(const std::string& id, const std::string& logical,
		const AssetSource& source, TextureCache& cache, std::string& error)
	{
		const RgbaImage* image = cache.Load(source, logical, error);
		if(image == nullptr)
			return false;
		Layer& layer = m_layers[id];
		layer.id = id;
		SplitLayerId(id, layer.major, layer.sub);
		layer.image = *image;
		layer.hasImage = true;
		layer.hasFill = false;
		layer.visible = true;
		layer.alpha = 255;
		return true;
	}

	void Stage::SetLayerColor(const std::string& id, const uint8_t rgba[4], int width,
		int height)
	{
		Layer& layer = m_layers[id];
		layer.id = id;
		SplitLayerId(id, layer.major, layer.sub);
		layer.hasImage = false;
		layer.hasFill = true;
		memcpy(layer.fill, rgba, 4);
		layer.fillWidth = width;
		layer.fillHeight = height;
		layer.visible = true;
		layer.alpha = 255;
	}

	bool Stage::LayerProperty(const std::string& id, const std::string& key,
		int& value) const
	{
		std::map<std::string, Layer>::const_iterator found = m_layers.find(id);
		if(found == m_layers.end())
			return false;
		const Layer& layer = found->second;
		if(key == "alpha")
			value = layer.alpha;
		else if(key == "top")
			value = layer.top;
		else if(key == "left")
			value = layer.left;
		else if(key == "visible")
			value = layer.visible ? 1 : 0;
		else
			return false;
		return true;
	}

	bool Stage::LayerBox(const std::string& id, int& left, int& top, int& width, int& height) const
	{
		std::map<std::string, Layer>::const_iterator found = m_layers.find(id);
		if(found == m_layers.end())
			return false;
		int ancestorLeft = 0, ancestorTop = 0;
		AncestorOffsets(m_layers, found->second.id, ancestorLeft, ancestorTop);
		left = found->second.left + ancestorLeft;
		top = found->second.top + ancestorTop;
		width = found->second.image.width;
		height = found->second.image.height;
		if(width <= 0 || height <= 0)
		{
			width = found->second.fillWidth;
			height = found->second.fillHeight;
		}
		return width > 0 && height > 0;
	}

	bool Stage::LayerAlphaAt(const std::string& id, int x, int y, int& alpha) const
	{
		std::map<std::string, Layer>::const_iterator found = m_layers.find(id);
		if(found == m_layers.end() || !found->second.hasImage)
			return false;
		const Layer& layer = found->second;
		int ancestorLeft = 0, ancestorTop = 0;
		AncestorOffsets(m_layers, layer.id, ancestorLeft, ancestorTop);
		const int localX = x - (layer.left + ancestorLeft);
		const int localY = y - (layer.top + ancestorTop);
		if(localX < 0 || localY < 0 || localX >= layer.image.width || localY >= layer.image.height)
			return false;
		const size_t at = ((size_t)localY * layer.image.width + localX) * 4 + 3;
		if(at >= layer.image.rgba.size())
			return false;
		alpha = layer.image.rgba[at];
		return true;
	}

	void Stage::DeleteLayerTree(const std::string& id)
	{
		if(id.empty())
			return;
		DeleteLayer(id);
		const std::string prefix = id + ".";
		for(std::map<std::string, Layer>::iterator it = m_layers.begin(); it != m_layers.end();)
		{
			if(it->first.compare(0, prefix.size(), prefix) == 0)
				it = m_layers.erase(it);
			else
				++it;
		}
	}

	bool Stage::SetProperty(const std::string& id, const std::string& key,
		const std::string& value)
	{
		std::map<std::string, Layer>::iterator found = m_layers.find(id);
		if(found == m_layers.end())
		{
			// a property on a layer that does not exist yet still has to be kept:
			// the real scripts set `top` before some layers are drawn
			Layer& layer = m_layers[id];
			layer.id = id;
			SplitLayerId(id, layer.major, layer.sub);
			found = m_layers.find(id);
		}
		Layer& layer = found->second;
		if(key == "alpha")
			layer.alpha = std::max(0, std::min(255, atoi(value.c_str())));
		else if(key == "top")
			layer.top = atoi(value.c_str());
		else if(key == "left")
			layer.left = atoi(value.c_str());
		else if(key == "visible")
			layer.visible = atoi(value.c_str()) != 0;
		else
		{
			m_ignoredProperties.insert(key);
			return false;
		}
		return true;
	}

	void Stage::DeleteLayer(const std::string& id) { m_layers.erase(id); }

	bool Stage::HasLayer(const std::string& id) const
	{
		return m_layers.find(id) != m_layers.end();
	}

	bool Stage::LayerSize(const std::string& id, int& width, int& height) const
	{
		std::map<std::string, Layer>::const_iterator found = m_layers.find(id);
		if(found == m_layers.end())
			return false;
		const Layer& layer = found->second;
		if(layer.hasImage)
		{
			width = layer.image.width;
			height = layer.image.height;
			return true;
		}
		if(layer.hasFill)
		{
			width = layer.fillWidth;
			height = layer.fillHeight;
			return true;
		}
		return false;
	}

	std::string Stage::DescribeLayers() const
	{
		std::string out;
		for(const std::pair<const std::string, Layer>& item : m_layers)
		{
			const Layer& layer = item.second;
			if(!out.empty())
				out += " | ";
			out += layer.id + ":";
			if(layer.hasImage)
				out += "image " + std::to_string(layer.image.width) + "x" +
					std::to_string(layer.image.height);
			else if(layer.hasFill)
				out += "fill " + std::to_string(layer.fill[0]) + "," +
					std::to_string(layer.fill[1]) + "," + std::to_string(layer.fill[2]) +
					" " + std::to_string(layer.fillWidth) + "x" +
					std::to_string(layer.fillHeight);
			else
				out += "empty";
			out += " alpha=" + std::to_string(layer.alpha) + " at " +
				std::to_string(layer.left) + "," + std::to_string(layer.top);
			if(!layer.visible)
				out += " hidden";
		}
		return out;
	}

	std::vector<std::string> Stage::layerIds() const
	{
		std::vector<std::string> out;
		for(const std::pair<const std::string, Layer>& item : m_layers)
			out.push_back(item.first);
		return out;
	}

	bool Stage::hasBackgroundImage() const
	{
		for(const std::pair<const std::string, Layer>& item : m_layers)
		{
			const Layer& layer = item.second;
			if(layer.hasImage && (layer.major == 0 || layer.major == 1) && layer.visible)
				return true;
		}
		return false;
	}

	TextStyle& Stage::Style(const std::string& name)
	{
		std::map<std::string, TextStyle>::iterator found = m_styles.find(name);
		if(found == m_styles.end())
			found = m_styles.insert(std::make_pair(name, TextStyle())).first;
		return found->second;
	}

	bool Stage::hasStyle(const std::string& name) const
	{
		return m_styles.find(name) != m_styles.end();
	}

	std::vector<std::string> Stage::styleNames() const
	{
		std::vector<std::string> out;
		for(const std::pair<const std::string, TextStyle>& item : m_styles)
			out.push_back(item.first);
		return out;
	}

	void Stage::NoteTransition(const std::string& type, int milliseconds)
	{
		m_transitionType = type;
		m_transitionMs = milliseconds;
		m_transitions++;
	}

	int Stage::TextOpacity() const
	{
		std::map<std::string, Layer>::const_iterator found = m_layers.find("80");
		if(found != m_layers.end())
			return std::max(0, std::min(255, found->second.alpha));
		return 255;
	}

	void Stage::DrawStyledText(Canvas& canvas, const TextStyle& style, const std::string& text,
		int alpha, size_t& pixels, int& glyphs, std::string& error)
	{
		if(text.empty() || m_fonts == nullptr)
			return;
		RgbaImage block;
		int count = 0;
		std::string reason;
		if(!m_fonts->Render(style, text, block, count, reason))
		{
			if(!error.empty())
				error += "; ";
			error += reason;
			return;
		}
		for(size_t i = 3; i < block.rgba.size(); i += 4)
			if(block.rgba[i] != 0)
				pixels++;
		glyphs += count;
		canvas.Blend(block.rgba.data(), block.width, block.height, style.left, style.top, alpha);
	}

	bool Stage::Composite(Canvas& canvas, bool includeText, std::string& error)
	{
		error.clear();
		canvas.Clear(0, 0, 0);
		std::vector<const Layer*> ordered;
		ordered.reserve(m_layers.size());
		for(const std::pair<const std::string, Layer>& item : m_layers)
			if(item.second.visible)
				ordered.push_back(&item.second);
		std::sort(ordered.begin(), ordered.end(), [](const Layer* a, const Layer* b) {
			if(a->major != b->major)
				return a->major < b->major;
			return a->sub < b->sub;
		});
		for(const Layer* layer : ordered)
		{
			int ancestorLeft = 0, ancestorTop = 0;
			AncestorOffsets(m_layers, layer->id, ancestorLeft, ancestorTop);
			const int offsetLeft = layer->left + ancestorLeft;
			const int offsetTop = layer->top + ancestorTop;
			if(layer->hasImage)
				canvas.Blend(layer->image.rgba.data(), layer->image.width, layer->image.height,
					offsetLeft, offsetTop, layer->alpha);
			else if(layer->hasFill)
			{
				uint8_t fill[4] = { layer->fill[0], layer->fill[1], layer->fill[2],
					(uint8_t)((unsigned)layer->fill[3] * (unsigned)layer->alpha / 255u) };
				canvas.Fill(layer->left + offsetLeft, layer->top + offsetTop,
					layer->fillWidth, layer->fillHeight, fill);
			}
		}
		m_lastTextPixels = 0;
		m_lastGlyphs = 0;
		if(includeText)
		{
			size_t pixels = 0;
			int glyphs = 0;
			DrawStyledText(canvas, Style(m_activeStyle), m_page.text, TextOpacity(), pixels, glyphs,
				error);
			if(!m_page.speaker.empty())
				DrawStyledText(canvas, Style("name"), m_page.speaker, 255, pixels, glyphs, error);
			m_lastTextPixels = pixels;
			m_lastGlyphs = glyphs;
		}
		return error.empty();
	}
}
