// screen.h - the parts of the engine that put a picture on the stage
//
// The layers below the scenario: assets that come out of the shipped archive (or
// an unpacked tree), a texture cache so a background is decoded once and not
// once per frame, the canvas that everything is composited into, the game's own
// TTF through CoreText, and the layer model the [lyc]/[lyprop]/[lydel] commands
// drive.
//
// Numbers this file does not invent:
//  * the stage size comes from the game's system.ini (1024x698 for the title
//    this tree is checked against);
//  * the dialogue box geometry and the font come from the game's first.iet
//    `[chgmsg id="adv"] [font ...]` block, read at boot.  The defaults here are
//    only what a script without a boot file would get, and the self test
//    asserts the real values were picked up from the real file;
//  * JPEG backgrounds are decoded with ImageIO (a system framework: no new
//    third-party dependency), PNG with pngio.

#ifndef ARTEMIS_RENDER_SCREEN_H
#define ARTEMIS_RENDER_SCREEN_H

#include "pfs.h"
#include "pngio.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace artemis
{
	struct RgbaImage
	{
		int width = 0;
		int height = 0;
		std::vector<uint8_t> rgba; // straight (non-premultiplied) alpha

		bool empty() const { return width <= 0 || height <= 0 || rgba.empty(); }
		size_t bytes() const { return rgba.size(); }
	};

	// -------------------------------------------------------------------------
	// assets
	// -------------------------------------------------------------------------
	//
	// A real game's members are named with backslashes and without extensions in
	// the script ("bgimage\milkeyway" is `bgimage/milkeyway.jpg` on disk), so a
	// lookup normalises the path and tries the extensions the release ships.
	class AssetSource
	{
	public:
		bool AddArchive(const std::string& path, std::string& error);
		bool AddDirectory(const std::string& path, std::string& error);

		bool empty() const { return m_archives.empty() && m_directories.empty(); }
		size_t archiveCount() const { return m_archives.size(); }
		size_t directoryCount() const { return m_directories.size(); }
		size_t memberCount() const;
		std::string Describe() const; // "root.pfs (pf6, 125 members) + extracted/"

		bool Exists(const std::string& logical) const;

		// Read one logical name; `resolved` is the concrete member or file that
		// answered, which is also what the texture cache keys on.
		bool Read(const std::string& logical, std::vector<uint8_t>& out, std::string& resolved,
			std::string& error) const;

	private:
		struct ArchiveSlot
		{
			std::string path;
			std::string magic;
			size_t members = 0;
			Archive archive;
		};
		struct DirectorySlot
		{
			std::string path;
			mutable bool indexed = false;
			mutable std::map<std::string, std::string> index; // lower-case name -> real path
		};

		bool ReadArchive(const ArchiveSlot& slot, const std::string& logical,
			std::vector<uint8_t>& out, std::string& resolved) const;
		bool ReadDirectory(const DirectorySlot& slot, const std::string& logical,
			std::vector<uint8_t>& out, std::string& resolved) const;
		static const std::vector<std::string>& Extensions();

		std::vector<ArchiveSlot> m_archives;
		std::vector<DirectorySlot> m_directories;
	};

	// Decode an image by content, not by name: PNG through pngio, JPEG through
	// ImageIO.  Returns false with a reason for anything else.
	bool DecodeImage(const uint8_t* data, size_t size, RgbaImage& out, std::string& error);

	// "FFFFFF", "0xFFFFFF" or "AARRGGBB" as the scripts spell colours.
	bool ParseColor(const std::string& text, uint8_t rgba[4]);

	// One entry per decoded image.  A frame that only moves text never decodes
	// again, and the archive member is read once.
	class TextureCache
	{
	public:
		const RgbaImage* Load(const AssetSource& source, const std::string& logical,
			std::string& error, std::string* resolvedOut = nullptr);

		size_t hits() const { return m_hits; }
		size_t misses() const { return m_misses; }
		// where the first frame's milliseconds went: reading members, then decoding
		double readMs() const { return m_readMs; }
		double decodeMs() const { return m_decodeMs; }
		size_t cachedBytes() const;
		size_t size() const { return m_entries.size(); }
		std::vector<std::string> names() const;

	private:
		struct Entry
		{
			RgbaImage image;
			std::string resolved;
		};
		std::map<std::string, Entry> m_entries;
		size_t m_hits = 0;
		size_t m_misses = 0;
		double m_readMs = 0;
		double m_decodeMs = 0;
	};

	// -------------------------------------------------------------------------
	// fonts
	// -------------------------------------------------------------------------
	//
	// The game ships its own TTF and names it in its own config; the font must be
	// loaded from those bytes, not from a system typeface, or the "same text as
	// the game" claim is empty.  CoreText is used through a pimpl so this header
	// stays free of framework types.
	struct TextStyle
	{
		std::string face = "GenShin-Mono-Normal.ttf";
		int left = 42;
		int top = 460;
		int width = 937;
		int height = 250;
		int size = 36;
		uint8_t color[3] = { 255, 255, 255 };
		uint8_t shadowColor[3] = { 0x12, 0x12, 0x12 };
		bool shadow = false;
		int align = 0; // 0 left, 1 center, 2 right
		int lineSpacing = 8;
	};

	class FontLibrary
	{
	public:
		FontLibrary();
		~FontLibrary();
		FontLibrary(const FontLibrary&) = delete;
		FontLibrary& operator=(const FontLibrary&) = delete;

		bool Load(const std::string& face, const std::vector<uint8_t>& data, std::string& error);
		bool Has(const std::string& face) const;
		std::vector<std::string> faces() const;
		std::string PostScriptName(const std::string& face) const;
		std::string Sha1Hex(const std::string& face) const; // the bytes the font came from
		size_t byteSize(const std::string& face) const;

		// Lay the text out inside style.width x style.height and return that bitmap
		// (transparent elsewhere).  `glyphs` counts the glyphs CoreText actually
		// laid out, which is the "the text was shaped, not stamped" number.
		bool Render(const TextStyle& style, const std::string& utf8, RgbaImage& out,
			int& glyphs, std::string& error);

	private:
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};

	// -------------------------------------------------------------------------
	// the canvas
	// -------------------------------------------------------------------------
	class Canvas
	{
	public:
		Canvas(int width, int height);

		int width() const { return m_width; }
		int height() const { return m_height; }
		const uint8_t* data() const { return m_pixels.data(); }
		uint8_t* data() { return m_pixels.data(); }
		size_t byteSize() const { return m_pixels.size(); }

		void Clear(uint8_t r, uint8_t g, uint8_t b);
		void Fill(int x, int y, int w, int h, const uint8_t rgba[4]);
		void Blend(const uint8_t* src, int srcWidth, int srcHeight, int left, int top,
			int alpha);

		uint32_t At(int x, int y) const; // 0xAARRGGBB
		size_t distinctColors() const;
		size_t differingPixels(const Canvas& other, int x, int y, int w, int h) const;

	private:
		int m_width = 0;
		int m_height = 0;
		std::vector<uint8_t> m_pixels;
	};

	// -------------------------------------------------------------------------
	// the stage: layers, styles, the current page of text
	// -------------------------------------------------------------------------
	struct Layer
	{
		std::string id;    // "0", "80.1", "name"
		int major = 0;     // the number before the dot
		int sub = 0;       // the number after it, 0 when there is none
		bool visible = true;
		int left = 0;
		int top = 0;
		int alpha = 255;
		bool hasImage = false;
		bool hasFill = false;
		RgbaImage image;
		uint8_t fill[4] = { 0, 0, 0, 255 };
		int fillWidth = 0;
		int fillHeight = 0;
	};

	struct PageText
	{
		std::string text;    // what the speaker says, newlines kept
		std::string speaker; // the name box, empty for narration
	};

	class Stage
	{
	public:
		Stage(int width, int height);

		int width() const { return m_width; }
		int height() const { return m_height; }
		void SetFonts(FontLibrary* fonts) { m_fonts = fonts; }

		bool SetLayerImage(const std::string& id, const std::string& logical,
			const AssetSource& source, TextureCache& cache, std::string& error);
		void SetLayerColor(const std::string& id, const uint8_t rgba[4], int width, int height);
		// `visible=0|1`, `alpha=0..255`, `top=`, `left=`; unknown keys are kept in
		// ignoredProperties() rather than silently dropped
		bool SetProperty(const std::string& id, const std::string& key, const std::string& value);
		// The value a layer carries right now ("alpha", "top", "left", "visible").
		// Read-only, and the reason it exists: a tween has to be *checked* where it
		// landed, not only seen in a picture.
		bool LayerProperty(const std::string& id, const std::string& key, int& value) const;
		// Where a layer is drawn, ancestors included, and the image's size.  This is
		// the rectangle a click has to land in, so it is the same arithmetic
		// Composite() uses rather than a second copy of it.
		bool LayerBox(const std::string& id, int& left, int& top, int& width, int& height) const;
		// The plate's own alpha at a stage point: `clickablethreshold="128"` says a
		// button is only clickable where it is actually opaque, and this is that test.
		bool LayerAlphaAt(const std::string& id, int x, int y, int& alpha) const;
		void DeleteLayer(const std::string& id);
		// `[lydel id="60"]` removes the *group* 60: the layer and every nested
		// child ("60.1", "60.1.1").  A title screen's buttons are children like
		// that, and deleting only the parent would leave them on the stage.
		void DeleteLayerTree(const std::string& id);
		bool HasLayer(const std::string& id) const;
		bool LayerSize(const std::string& id, int& width, int& height) const;
		size_t layerCount() const { return m_layers.size(); }
		std::vector<std::string> layerIds() const;
		std::string DescribeLayers() const; // for the report: what is on stage, where
		bool hasBackgroundImage() const; // a layer `0` or `1` holding decoded pixels

		void SetPage(const PageText& page) { m_page = page; }
		void SetSpeaker(const std::string& speaker) { m_page.speaker = speaker; }
		void SetPageText(const std::string& text) { m_page.text = text; }
		const PageText& page() const { return m_page; }
		void ClearPage() { m_page = PageText(); }

		TextStyle& Style(const std::string& name);
		bool hasStyle(const std::string& name) const;
		std::vector<std::string> styleNames() const;
		void SetActiveStyle(const std::string& name) { m_activeStyle = name; }
		const std::string& activeStyle() const { return m_activeStyle; }

		void NoteTransition(const std::string& type, int milliseconds);
		const std::string& lastTransitionType() const { return m_transitionType; }
		int lastTransitionMs() const { return m_transitionMs; }
		int transitionCount() const { return m_transitions; }

		// `includeText == false` is the same picture without the words, which is how
		// the self test measures "these pixels are the text" from the framebuffer.
		bool Composite(Canvas& canvas, bool includeText, std::string& error);

		size_t lastTextPixels() const { return m_lastTextPixels; }
		int lastGlyphs() const { return m_lastGlyphs; }
		const std::set<std::string>& ignoredProperties() const { return m_ignoredProperties; }

	private:
		void DrawStyledText(Canvas& canvas, const TextStyle& style, const std::string& text,
			int alpha, size_t& pixels, int& glyphs, std::string& error);
		int TextOpacity() const;

		int m_width = 0;
		int m_height = 0;
		std::map<std::string, Layer> m_layers;
		std::map<std::string, TextStyle> m_styles;
		std::string m_activeStyle = "adv";
		PageText m_page;
		FontLibrary* m_fonts = nullptr;
		std::string m_transitionType = "0";
		int m_transitionMs = 0;
		int m_transitions = 0;
		size_t m_lastTextPixels = 0;
		int m_lastGlyphs = 0;
		std::set<std::string> m_ignoredProperties;
	};
}

#endif // ARTEMIS_RENDER_SCREEN_H
