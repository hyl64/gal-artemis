// ipt.cpp - parse every IPT dialect and composite the one that draws
//
// The descriptor syntax is the Lua-table dialect in src/lua.h; this file decides
// which of the four real dialects a file is, and draws the composite one.
//
// The compositor follows reference/ImageIPT.cs, not intuition: the base goes at
// (0,0) inside the canvas (base.x/y are the image's world position, exported as
// ImageMetaData.OffsetX/Y), diff canvases are Bgr32 and therefore fully opaque,
// and cut canvases are Bgra32 and start transparent.  See ipt.h for the longer
// reasoning and the real descriptor that pins it.

#include "ipt.h"
#include "lua.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace artemis
{
	const char* Name(IptDialect dialect)
	{
		switch(dialect)
		{
		case IptDialect::Unknown: return "unknown";
		case IptDialect::Atlas: return "atlas";
		case IptDialect::Data: return "data";
		case IptDialect::Anime: return "anime";
		case IptDialect::Composite: return "composite";
		}
		return "?";
	}

	namespace
	{
		// A descriptor is a text file from a game, not a trusted input: a damaged
		// header must fail with a message, not try to allocate 4 GB.
		constexpr int kMaxCanvasDimension = 16384;
		constexpr int64_t kMaxCanvasBytes = 512LL << 20; // 512 MiB of RGBA

		// A canvas declared as 4294967296 must be refused, not silently wrapped:
		// the conversion from a double outside int's range is undefined in C++.
		bool IntValue(const LuaValue& value, int& out)
		{
			if(value.kind != LuaValue::Kind::Integer && value.kind != LuaValue::Kind::Number)
				return false;
			const double v = value.asNumber;
			if(!(v >= -2147483648.0 && v <= 2147483647.0))
				return false;
			out = (int)v;
			return true;
		}

		bool IntField(const LuaValue& object, const char* name, int& out)
		{
			const LuaValue* value = object.field(name);
			return value != nullptr && IntValue(*value, out);
		}

		bool StringField(const LuaValue& object, const char* name, std::string& out)
		{
			const LuaValue* value = object.field(name);
			if(value == nullptr || value->kind != LuaValue::Kind::String)
				return false;
			out = value->text;
			return true;
		}

		// "576,0,424,36" -> a rectangle.  The atlas dialect keeps this as text;
		// splitting it belongs to the layer that draws the sprite sheet.
		bool ParseRect(const std::string& text, IptRect& out)
		{
			int values[4] = { 0, 0, 0, 0 };
			size_t at = 0;
			for(int i = 0; i < 4; i++)
			{
				while(at < text.size() && (text[at] == ' ' || text[at] == '\t'))
					at++;
				const size_t start = at;
				if(at < text.size() && (text[at] == '-' || text[at] == '+'))
					at++;
				while(at < text.size() && text[at] >= '0' && text[at] <= '9')
					at++;
				if(at == start || (at == start + 1 && (text[start] == '-' || text[start] == '+')))
					return false;
				values[i] = (int)strtol(text.substr(start, at - start).c_str(), nullptr, 10);
				while(at < text.size() && (text[at] == ' ' || text[at] == '\t'))
					at++;
				if(i < 3)
				{
					if(at >= text.size() || text[at] != ',')
						return false;
					at++;
				}
			}
			while(at < text.size() && (text[at] == ' ' || text[at] == '\t'))
				at++;
			if(at != text.size())
				return false;
			out.x = values[0];
			out.y = values[1];
			out.w = values[2];
			out.h = values[3];
			return true;
		}

		bool EndsWithPng(const std::string& name)
		{
			if(name.size() < 4)
				return false;
			const char* tail = name.c_str() + name.size() - 4;
			return tail[0] == '.'
				&& (tail[1] == 'p' || tail[1] == 'P')
				&& (tail[2] == 'n' || tail[2] == 'N')
				&& (tail[3] == 'g' || tail[3] == 'G');
		}

		// `iettool info page.png` must say what the file is, not complain about an
		// invisible character somewhere in the binary.
		bool LooksBinary(const std::string& text)
		{
			static const unsigned char png[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
			if(text.size() >= sizeof(png) && memcmp(text.data(), png, sizeof(png)) == 0)
				return true;
			const size_t limit = std::min<size_t>(text.size(), 256);
			for(size_t i = 0; i < limit; i++)
				if(text[i] == '\0')
					return true;
			return false;
		}

		// The descriptors name their PNGs without the extension, but a writer that
		// includes it must not turn into "a.png.png".
		std::string PngName(const std::string& name)
		{
			return EndsWithPng(name) ? name : name + ".png";
		}

		std::string JoinPath(const std::string& directory, const std::string& name)
		{
			if(directory.empty())
				return name;
			if(directory.back() == '/')
				return directory + name;
			return directory + "/" + name;
		}

		bool CanvasLimits(int width, int height, std::string& error)
		{
			if(width > kMaxCanvasDimension || height > kMaxCanvasDimension
				|| (int64_t)width * (int64_t)height * 4 > kMaxCanvasBytes)
			{
				error = "IPT canvas " + std::to_string(width) + "x" + std::to_string(height)
					+ " is too large to decode";
				return false;
			}
			return true;
		}

		void ParsePoints(const LuaValue& root, Ipt& out)
		{
			for(size_t i = 0; i < root.items.size(); i++)
			{
				const LuaValue& item = root.items[i];
				if(!item.isObject())
					continue;
				int x = 0, y = 0;
				const bool hasX = IntField(item, "x", x);
				const bool hasY = IntField(item, "y", y);
				if(hasX && hasY)
					out.points.push_back(IptPoint{ x, y });
				else if(hasX || hasY)
					out.notes.push_back("array item " + std::to_string(i)
						+ " has only one coordinate; not counted as a point");
			}
		}

		// trial = { ruri = {1,1,5}, ... } and main = { ... }, in file order.
		void ParseGroups(const LuaValue& root, Ipt& out)
		{
			for(const auto& field : root.entries)
			{
				if(!field.second.isObject())
					continue;
				IptGroup group;
				group.name = field.first;
				for(const auto& member : field.second.entries)
				{
					IptGroupEntry entry;
					entry.name = member.first;
					if(member.second.isObject())
					{
						for(const LuaValue& value : member.second.items)
						{
							int number = 0;
							if(IntValue(value, number))
								entry.values.push_back(number);
						}
					}
					else
					{
						int number = 0;
						if(IntValue(member.second, number))
							entry.values.push_back(number);
					}
					if(!entry.values.empty())
						group.entries.push_back(entry);
				}
				if(!group.entries.empty())
					out.groups.push_back(group);
			}
		}

		bool CollectTiles(const LuaValue& root, Ipt& out, std::string& error)
		{
			auto add = [&](const LuaValue& value, const std::string& namedField) {
				if(!value.isObject())
					return true;
				const LuaValue* file = value.field("file");
				if(file == nullptr || file->kind != LuaValue::Kind::String)
					return true;
				IptTile tile;
				tile.order = out.tiles.size();
				tile.file = file->text;
				if(tile.file.empty())
				{
					error = "composite IPT has a tile with an empty file name";
					return false;
				}
				tile.hasId = IntField(value, "id", tile.id);
				if(!tile.hasId)
					tile.id = (int)tile.order;
				IntField(value, "x", tile.x);
				IntField(value, "y", tile.y);
				if(!namedField.empty())
					out.notes.push_back("took tile '" + tile.file + "' from named field '"
						+ namedField + "'");
				out.tiles.push_back(tile);
				return true;
			};
			for(const LuaValue& value : root.items)
				if(!add(value, ""))
					return false;
			for(const auto& field : root.entries)
			{
				if(field.first == "base" || field.first == "mode")
					continue;
				if(!add(field.second, field.first))
					return false;
			}
			return true;
		}
	}

	// ----------------------------------------------------------- the dialects

	bool ParseIpt(const std::string& text, Ipt& out, std::string& error)
	{
		out = Ipt{}; // a caller may reuse one Ipt across files
		if(LooksBinary(text))
		{
			error = "the file is binary (a PNG image or other payload)";
			return false;
		}
		LuaValue root;
		std::string firstKey;
		if(!ParseLuaTable(text, root, error, &firstKey))
			return false;
		const LuaValue* ipt = root.field("ipt");
		if(ipt == nullptr)
		{
			error = "no 'ipt' table (the file opens with '" + firstKey + "')";
			return false;
		}
		if(!ipt->isObject())
		{
			error = "the 'ipt' table is not an object";
			return false;
		}
		// copy out of the parsed table before replacing it: assigning `root` from a
		// value that lives inside `root` would read freed storage
		LuaValue table = *ipt;
		root = std::move(table);

		const LuaValue* mode = root.field("mode");
		if(mode != nullptr)
		{
			if(mode->kind == LuaValue::Kind::String)
				out.mode = mode->text;
			else
				out.notes.push_back("'mode' is a " + std::string(Name(mode->kind))
					+ ", not a string");
		}

		const LuaValue* base = root.field("base");
		if(base != nullptr && base->isObject())
		{
			out.hasBase = true;
			IntField(*base, "w", out.width);
			IntField(*base, "h", out.height);
			IntField(*base, "x", out.baseX);
			IntField(*base, "y", out.baseY);
			IntField(*base, "ax", out.anchorX);
			IntField(*base, "ay", out.anchorY);
			IntField(*base, "time", out.frameTime);
			IntField(*base, "max", out.frameMax);
			IntField(*base, "loop", out.frameLoop);
			if(const std::string* name = base->firstString())
				out.base = *name;
			else
				StringField(*base, "file", out.base); // a writer that uses `file`
		}

		if(out.mode == "cut" || out.mode == "diff")
		{
			out.dialect = IptDialect::Composite;
			if(!CollectTiles(root, out, error))
				return false;
			if(out.width <= 0 || out.height <= 0)
			{
				error = "composite IPT with no canvas size";
				return false;
			}
			if(!CanvasLimits(out.width, out.height, error))
				return false;
			if(out.mode == "diff" && out.base.empty())
			{
				error = "diff mode needs a base image name and there is none";
				return false;
			}
			if(out.mode == "cut" && out.tiles.empty())
			{
				error = "cut mode with no tiles has nothing to draw";
				return false;
			}
			return true;
		}

		if(out.mode.rfind("anime", 0) == 0)
		{
			out.dialect = IptDialect::Anime;
			if(out.mode != "anime_full")
				out.notes.push_back("anime mode '" + out.mode
					+ "' is read like anime_full");
			if(out.width <= 0 || out.height <= 0)
			{
				error = "anime IPT with no base canvas size";
				return false;
			}
			if(!CanvasLimits(out.width, out.height, error))
				return false;
			for(const LuaValue& value : root.items)
			{
				if(value.kind == LuaValue::Kind::String)
					out.frames.push_back(value.text);
				else if(value.isObject())
				{
					if(const std::string* name = value.firstString())
						out.frames.push_back(*name);
				}
			}
			for(const auto& field : root.entries)
			{
				if(field.first == "base" || field.first == "mode")
					continue;
				if(field.second.kind == LuaValue::Kind::String)
				{
					out.frames.push_back(field.second.text);
					out.notes.push_back("took frame from named field '" + field.first + "'");
				}
				else if(field.second.isObject())
				{
					if(const std::string* name = field.second.firstString())
					{
						out.frames.push_back(*name);
						out.notes.push_back("took frame from named field '" + field.first + "'");
					}
				}
			}
			return true;
		}

		if(!out.mode.empty())
		{
			// a mode we do not know is reported, not guessed at
			out.dialect = IptDialect::Unknown;
			out.notes.push_back("unrecognised IPT mode '" + out.mode + "'");
			return true;
		}

		// no mode: a sprite-sheet atlas, or a table of points
		size_t namedStrings = 0;
		for(const auto& field : root.entries)
		{
			if(field.second.kind != LuaValue::Kind::String)
				continue;
			namedStrings++;
			IptRect rect;
			if(ParseRect(field.second.text, rect))
			{
				rect.name = field.first;
				out.rects.push_back(rect);
			}
		}
		ParsePoints(root, out);
		ParseGroups(root, out);

		// A rectangle table may carry one or two non-rectangle strings (a version,
		// a sheet name); the points table has no named strings at all.  Majority
		// decides, so neither real shape is one odd field away from flipping.
		const bool rectMajority = namedStrings > 0
			&& out.rects.size() * 2 >= namedStrings;
		if(rectMajority)
			out.dialect = IptDialect::Atlas;
		else if(!out.points.empty() || !out.groups.empty())
			out.dialect = IptDialect::Data;
		else
		{
			out.dialect = IptDialect::Unknown;
			out.notes.push_back("the table is neither rectangles nor points");
		}
		if(out.dialect == IptDialect::Atlas && out.rects.size() < namedStrings)
			out.notes.push_back(std::to_string(namedStrings - out.rects.size())
				+ " named string(s) are not x,y,w,h rectangles");
		if(out.dialect == IptDialect::Data && !out.rects.empty())
			out.notes.push_back("the table also holds "
				+ std::to_string(out.rects.size()) + " rectangle-like string(s)");
		return true;
	}

	// ----------------------------------------------------------- compositing

	void BlendPixel(uint8_t* dst, const uint8_t* src)
	{
		const uint8_t sa = src[3];
		if(sa == 0)
			return;
		if(sa == 0xFF || dst[3] == 0)
		{
			dst[0] = src[0];
			dst[1] = src[1];
			dst[2] = src[2];
		}
		else
		{
			dst[0] = (uint8_t)((src[0] * sa + dst[0] * (0xFF - sa)) / 0xFF);
			dst[1] = (uint8_t)((src[1] * sa + dst[1] * (0xFF - sa)) / 0xFF);
			dst[2] = (uint8_t)((src[2] * sa + dst[2] * (0xFF - sa)) / 0xFF);
		}
		dst[3] = sa;
	}

	namespace
	{
		// diff mode's canvas is Bgr32: it has no alpha channel to write, so the
		// blend changes colour only and the result stays opaque.
		void BlendRgbKeepAlpha(uint8_t* dst, const uint8_t* src)
		{
			const uint8_t sa = src[3];
			if(sa == 0)
				return;
			if(sa == 0xFF)
			{
				dst[0] = src[0];
				dst[1] = src[1];
				dst[2] = src[2];
				return;
			}
			dst[0] = (uint8_t)((src[0] * sa + dst[0] * (0xFF - sa)) / 0xFF);
			dst[1] = (uint8_t)((src[1] * sa + dst[1] * (0xFF - sa)) / 0xFF);
			dst[2] = (uint8_t)((src[2] * sa + dst[2] * (0xFF - sa)) / 0xFF);
		}

		enum class DrawMode
		{
			CopyOpaque, // base, or a tile with no alpha channel
			CutBlend,   // Bgra32 canvas: colour and alpha blend
			DiffBlend,  // Bgr32 canvas: colour blends, alpha stays 255
		};

		// Draw one tile into the canvas, clipping whatever falls outside.  The
		// negative origin is clipped, never translated: a tile at (-2,-2) loses
		// its left and top two pixels instead of moving to (0,0).
		bool DrawInto(Image& canvas, const Image& tile, int x, int y, DrawMode mode)
		{
			if(tile.width <= 0 || tile.height <= 0 || canvas.width <= 0 || canvas.height <= 0)
				return false;
			int sx = 0, sy = 0, dx = x, dy = y;
			if(x < 0)
			{
				sx = -x;
				dx = 0;
			}
			if(y < 0)
			{
				sy = -y;
				dy = 0;
			}
			if(sx >= tile.width || sy >= tile.height
				|| dx >= canvas.width || dy >= canvas.height)
				return false;
			const int width = std::min(tile.width - sx, canvas.width - dx);
			const int height = std::min(tile.height - sy, canvas.height - dy);
			if(width <= 0 || height <= 0)
				return false;
			const bool blendAlpha = (mode == DrawMode::CutBlend || mode == DrawMode::DiffBlend)
				&& tile.hasAlpha;
			for(int row = 0; row < height; row++)
			{
				const uint8_t* src = tile.rgba.data()
					+ ((size_t)(sy + row) * tile.width + sx) * 4;
				uint8_t* dst = canvas.rgba.data()
					+ ((size_t)(dy + row) * canvas.width + dx) * 4;
				if(!blendAlpha)
				{
					// the reference copies a tile with no alpha channel, and lays
					// the base down, with the canvas left fully opaque
					for(int col = 0; col < width; col++, src += 4, dst += 4)
					{
						dst[0] = src[0];
						dst[1] = src[1];
						dst[2] = src[2];
						dst[3] = 0xFF;
					}
				}
				else if(mode == DrawMode::CutBlend)
				{
					for(int col = 0; col < width; col++, src += 4, dst += 4)
						BlendPixel(dst, src);
				}
				else
				{
					for(int col = 0; col < width; col++, src += 4, dst += 4)
						BlendRgbKeepAlpha(dst, src);
				}
			}
			return true;
		}
	}

	bool ComposeIpt(const Ipt& ipt, const std::string& directory,
		const ResourceReader& read, Image& canvas, ComposeReport& report,
		std::string& error)
	{
		if(ipt.dialect != IptDialect::Composite)
		{
			error = std::string("this IPT is a '") + Name(ipt.dialect)
				+ "' descriptor, not a composite image";
			return false;
		}
		if(ipt.mode != "cut" && ipt.mode != "diff")
		{
			error = "unsupported IPT mode '" + ipt.mode + "'";
			return false;
		}
		if(ipt.width <= 0 || ipt.height <= 0)
		{
			error = "composite IPT with no canvas size";
			return false;
		}
		if(!CanvasLimits(ipt.width, ipt.height, error))
			return false;

		std::string dir = directory;
		if(!dir.empty() && dir.back() != '/')
			dir.push_back('/');

		const bool diff = ipt.mode == "diff";
		canvas = Image{};
		canvas.width = ipt.width;
		canvas.height = ipt.height;
		canvas.hasAlpha = !diff; // Bgra32 for cut, Bgr32 for diff
		canvas.rgba.assign((size_t)ipt.width * ipt.height * 4, 0);
		if(diff)
		{
			// a Bgr32 bitmap that was never written is black, not transparent
			for(size_t at = 3; at < canvas.rgba.size(); at += 4)
				canvas.rgba[at] = 0xFF;
		}

		if(diff)
		{
			if(ipt.base.empty())
			{
				error = "diff mode needs a base image name and there is none";
				return false;
			}
			const std::string path = JoinPath(dir, PngName(ipt.base));
			std::vector<uint8_t> bytes;
			std::string why;
			Image base;
			if(!read(path, bytes, why))
			{
				error = "cannot read base '" + path + "': " + why;
				return false;
			}
			if(!ReadPng(bytes.data(), bytes.size(), base, why))
			{
				error = "base '" + path + "' is not a readable PNG: " + why;
				return false;
			}
			// (0,0), not (base.x,base.y): see the header comment
			if(!DrawInto(canvas, base, 0, 0, DrawMode::CopyOpaque))
				report.notes.push_back("the base image does not intersect the canvas");
		}

		for(const IptTile& tile : ipt.tiles)
		{
			const std::string path = JoinPath(dir, PngName(tile.file));
			std::vector<uint8_t> bytes;
			std::string why;
			if(!read(path, bytes, why))
			{
				report.tilesSkipped++;
				report.missing.push_back(path + ": " + why);
				continue;
			}
			Image tileImage;
			if(!ReadPng(bytes.data(), bytes.size(), tileImage, why))
			{
				// present but not a PNG: a decoder error, not a packaging one
				error = "tile '" + path + "' is not a readable PNG: " + why;
				return false;
			}
			const DrawMode mode = diff ? DrawMode::DiffBlend : DrawMode::CutBlend;
			if(DrawInto(canvas, tileImage, tile.x, tile.y, mode))
				report.tilesDrawn++;
			else
			{
				report.tilesSkipped++;
				report.notes.push_back("tile '" + tile.file + "' at " + std::to_string(tile.x)
					+ "," + std::to_string(tile.y) + " does not intersect the canvas");
			}
		}
		return true;
	}
}
