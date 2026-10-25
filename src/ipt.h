// ipt.h - the IPT descriptor, in all four dialects the real data has
//
// `.ipt` is not one format, and the extension lies across titles (the same
// extension is a KAG script in one game - see docs/REALDATA.md).  Inside a
// title, four dialects appear, and only the fourth is a composite image:
//
//   atlas      ipt = { ok_qs = "576,0,424,36", ["瑠莉"] = "0,1,166,93" }
//              name -> "x,y,w,h" rectangles in a sprite sheet.  No pixels here.
//   data       ipt = { { x=50, y=394 }, trial = { ruri = { 1, 1, 5 } } }
//              map hot spots and route tables.  No pixels here either.
//   anime      ipt = { mode = "anime_full",
//                      base = { x=0, y=0, w=1280, h=720, ax=640, ay=360,
//                               time=100, max=3, loop=-1 },
//                      {"line11"}, {"line12"}, {"line13"} }
//              a frame sequence for the animation player.
//   composite  ipt = { mode = "cut"|"diff",
//                      base = { "ev01_base", w = 1280, h = 720, x = 0, y = 0 },
//                      { id = 0, file = "ev01_00", x = 0, y = 0 } }
//              the only dialect that draws anything, and what GARbro's
//              ImageIPT.cs describes.
//
// The dialect is decided from the parsed contents, never from the extension,
// and `iettool info` says which one it found - the parser and the image
// compositor are separate capabilities on purpose.
//
// Compositing rules (composite dialect only), from reference/ImageIPT.cs:
//
//   cut   canvas is Bgra32 and starts transparent; tiles are alpha-blended in,
//         in file order (their `id` is metadata, not a sort key - the reference
//         iterates the array and even carries a `// XXX order by Id?` note)
//   diff  canvas is Bgr32 and starts opaque black; the base PNG is laid down at
//         (0, 0) first, then tiles are alpha-blended over it in file order
//
// The base is *not* drawn at (base.x, base.y).  Those two fields are the
// composed image's on-screen offset: the reference stores them as
// ImageMetaData.OffsetX/OffsetY and calls ReadIntoCanvas (base_name, canvas, 0,
// 0).  Geometry agrees with that reading: a descriptor's canvas is exactly the
// base image's w/h, and a zev descriptor (base 3000x1796 at x=-700, y=-448,
// tile at 1391,447) would clip its own base and then draw the tile in a
// different coordinate space if the offset were applied inside the canvas.
//
//   sa = source alpha; if sa == 255 or the destination is transparent, the tile
//   colour is copied, otherwise it is (src*sa + dst*(255-sa))/255.  In cut mode
//   the destination alpha becomes sa (the reference writes it); in diff mode the
//   canvas has no alpha channel at all, so the result stays fully opaque.

#ifndef ARTEMIS_IPT_H
#define ARTEMIS_IPT_H

#include "pngio.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace artemis
{
	enum class IptDialect
	{
		Unknown,
		Atlas,     // sprite-sheet rectangles
		Data,      // map hot spots / route tables
		Anime,     // animation frame sequence
		Composite, // tiles composited onto a canvas
	};

	const char* Name(IptDialect dialect);

	struct IptTile
	{
		int id = 0;
		bool hasId = false; // the descriptor wrote `id = ...`
		std::string file;   // the tile name without ".png"
		int x = 0;
		int y = 0;
		size_t order = 0;   // where it appeared in the file; the draw order
	};

	struct IptRect // one sprite-sheet rectangle, atlas dialect
	{
		std::string name;
		int x = 0, y = 0, w = 0, h = 0;
	};

	struct IptPoint // one map hot spot, data dialect
	{
		int x = 0, y = 0;
	};

	struct IptGroupEntry // one character's route through a group
	{
		std::string name;
		std::vector<int> values;
	};

	struct IptGroup // `trial` / `main` in the real map descriptor
	{
		std::string name;
		std::vector<IptGroupEntry> entries;
	};

	struct Ipt
	{
		IptDialect dialect = IptDialect::Unknown;
		std::string mode;     // "cut", "diff", "anime_full", ...
		std::string base;     // base name without ".png" (diff mode)
		int width = 0;
		int height = 0;
		int baseX = 0;        // the composed image's on-screen offset, metadata
		int baseY = 0;        // (never applied inside the canvas, see above)
		int anchorX = 0;      // base.ax / base.ay, kept as metadata
		int anchorY = 0;
		bool hasBase = false;
		int frameTime = 0;    // anime: base.time
		int frameMax = 0;     // anime: base.max
		int frameLoop = 0;    // anime: base.loop
		std::vector<IptTile> tiles;        // composite dialect
		std::vector<IptRect> rects;        // atlas dialect
		std::vector<IptPoint> points;      // data dialect
		std::vector<IptGroup> groups;      // data dialect
		std::vector<std::string> frames;   // anime dialect
		std::vector<std::string> notes;    // lenient readings worth reporting
	};

	// Parse the text form and decide the dialect; never uses the extension.
	bool ParseIpt(const std::string& text, Ipt& out, std::string& error);

	// Where tile pixels come from: a file on disk, or a member of an archive.
	// The path handed in is the tile's logical path, e.g. "root/image/ev/a.png".
	using ResourceReader = std::function<bool(const std::string& path,
		std::vector<uint8_t>& out, std::string& error)>;

	struct ComposeReport
	{
		int tilesDrawn = 0;
		int tilesSkipped = 0;      // no intersection with the canvas
		std::vector<std::string> missing; // tile resources that could not be read
		std::vector<std::string> notes;
		// Filled by whoever times the two halves.  Kept here so a benchmark can
		// tell the compositor from the PNG encoder: on a real 1668x998 event CG the
		// encode is three orders of magnitude larger, and a single number would
		// blame the wrong half.
		long long composeUs = 0;
		long long encodeUs = 0;
	};

	// Assemble the canvas from a composite descriptor.  `directory` is where the
	// descriptor lives, inside whatever namespace the reader serves: "" for a
	// descriptor beside its tiles, "root/image/ev/" for one inside an archive.
	//
	// A tile whose resource is absent is reported in report.missing and skipped
	// (the image is still returned); a resource that is present but is not a
	// readable PNG is a hard error, because silently drawing nothing there would
	// hide a decoder bug.  A caller that needs the missing tiles to be fatal (the
	// CLI does - a real game must not ship a half image as success) checks
	// report.missing.
	bool ComposeIpt(const Ipt& ipt, const std::string& directory,
		const ResourceReader& read, Image& canvas, ComposeReport& report,
		std::string& error);

	// The blend rule, in one place so the tests can pin it.  This is the Bgra32
	// (cut-mode) rule: the destination alpha becomes the source alpha.
	void BlendPixel(uint8_t* dst, const uint8_t* src);
}

#endif // ARTEMIS_IPT_H
