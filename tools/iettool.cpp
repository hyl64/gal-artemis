// iettool - turn an Artemis IPT descriptor into a PNG
//
//   iettool info    <file.ipt>
//   iettool dump    <file.ipt>            every parsed field, for hand checking
//   iettool decode  <file.ipt> <out.png>
//   iettool decode  --archive <archive.pfs|.pfs.NNN> <member.ipt> <out.png>
//   iettool selftest                      fixtures, real descriptors, pixels
//
// The descriptor (`ipt = { mode = "cut"/"diff", base = {...}, tiles }`) and the
// tiles it names live next to each other - on disk, or together inside one .pfs
// member directory.  Both are served by the same compositor through a small
// reader callback, so a real archive needs no extraction step.
//
// The self test needs no game pixels: it writes its own PNG tiles with libpng,
// writes or copies descriptor text, decodes, and checks pixels against values
// worked out by hand.  It also runs the real descriptors this repository ships
// through the parser (structure only - the adult CG payloads in the henpri
// archive are deliberately never read) and pins their counts, canvases, tile
// coordinates and atlas rectangles.  A missing tile is a hard error for the CLI;
// a tile that exists but is not a PNG is a hard error everywhere.

#include "../src/ipt.h"
#include "../src/pfs.h"
#include "../src/pngio.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#ifdef ARTEMIS_TESTDATA
static const char* kTestData = ARTEMIS_TESTDATA;
#else
static const char* kTestData = "../testdata";
#endif

static std::string SamplePath(const char* relative)
{
	return std::string(kTestData) + "/" + relative;
}

static bool FileExists(const std::string& path)
{
	struct stat status;
	return stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode);
}

static std::vector<uint8_t> ReadFile(const std::string& path)
{
	FILE* f = fopen(path.c_str(), "rb");
	if(f == nullptr)
		return {};
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> out((size_t)(size > 0 ? size : 0));
	if(size > 0)
		out.resize(fread(out.data(), 1, out.size(), f));
	fclose(f);
	return out;
}

static bool ReadText(const std::string& path, std::string& out)
{
	const std::vector<uint8_t> bytes = ReadFile(path);
	if(bytes.empty())
		return false;
	out.assign((const char*)bytes.data(), bytes.size());
	return true;
}

static bool WriteFile(const std::string& path, const std::vector<uint8_t>& data)
{
	FILE* f = fopen(path.c_str(), "wb");
	if(f == nullptr)
		return false;
	if(!data.empty())
		fwrite(data.data(), 1, data.size(), f);
	fclose(f);
	return true;
}

static bool WriteText(const std::string& path, const std::string& text)
{
	return WriteFile(path, std::vector<uint8_t>(text.begin(), text.end()));
}

static std::string DirName(const std::string& path)
{
	const size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

static int Fail(const std::string& message)
{
	fflush(stdout); // keep the error after the progress it belongs to
	fprintf(stderr, "iettool: %s\n", message.c_str());
	return 1;
}

// ------------------------------------------------------------ resource readers

// tiles next to the descriptor, on disk
static artemis::ResourceReader DirReader(const std::string& base)
{
	return [base](const std::string& path, std::vector<uint8_t>& out, std::string& error) {
		const std::string full = base.empty() ? path : base + "/" + path;
		out = ReadFile(full);
		if(out.empty())
		{
			error = "no such file";
			return false;
		}
		return true;
	};
}

// tiles beside the descriptor, inside an archive (game paths use backslashes)
static artemis::ResourceReader ArchiveReader(const artemis::Archive& archive)
{
	return [&archive](const std::string& path, std::vector<uint8_t>& out, std::string& error) {
		const artemis::Entry* entry = artemis::Find(archive, path);
		if(entry == nullptr)
		{
			error = "no such member";
			return false;
		}
		out = artemis::Content(archive, *entry);
		return true;
	};
}

// ---------------------------------------------------------------------- info

static int Info(const std::string& path)
{
	const std::vector<uint8_t> bytes = ReadFile(path);
	if(bytes.empty())
		return Fail("cannot read " + path);
	artemis::Ipt ipt;
	std::string error;
	if(!artemis::ParseIpt(std::string((const char*)bytes.data(), bytes.size()), ipt, error))
		return Fail(path + ": not an IPT descriptor: " + error);
	printf("%s\n", path.c_str());
	printf("  dialect   %s\n", artemis::Name(ipt.dialect));
	printf("  mode      %s\n", ipt.mode.empty() ? "(none)" : ipt.mode.c_str());
	if(ipt.dialect == artemis::IptDialect::Composite
		|| ipt.dialect == artemis::IptDialect::Anime || ipt.hasBase)
		printf("  canvas    %d x %d (image origin %d,%d, anchor %d,%d)\n",
			ipt.width, ipt.height, ipt.baseX, ipt.baseY, ipt.anchorX, ipt.anchorY);
	printf("  base      %s\n", ipt.base.empty() ? "(none)" : ipt.base.c_str());
	printf("  tiles     %d\n", (int)ipt.tiles.size());
	for(const artemis::IptTile& t : ipt.tiles)
		printf("    id %-4d  %-32s at %d,%d\n", t.id, t.file.c_str(), t.x, t.y);
	if(!ipt.rects.empty())
	{
		printf("  rects     %d (name -> x,y,w,h)\n", (int)ipt.rects.size());
		for(size_t i = 0; i < ipt.rects.size() && i < 6; i++)
			printf("    %-16s %d,%d,%d,%d\n", ipt.rects[i].name.c_str(), ipt.rects[i].x,
				ipt.rects[i].y, ipt.rects[i].w, ipt.rects[i].h);
		if(ipt.rects.size() > 6)
			printf("    ... %d more\n", (int)(ipt.rects.size() - 6));
	}
	if(!ipt.points.empty())
		printf("  points    %d with x/y\n", (int)ipt.points.size());
	if(!ipt.groups.empty())
	{
		printf("  groups    %d\n", (int)ipt.groups.size());
		for(const artemis::IptGroup& group : ipt.groups)
			printf("    %-12s %d character route(s)\n", group.name.c_str(),
				(int)group.entries.size());
	}
	if(!ipt.frames.empty())
	{
		printf("  frames    %d (time %d, max %d, loop %d)\n", (int)ipt.frames.size(),
			ipt.frameTime, ipt.frameMax, ipt.frameLoop);
		for(size_t i = 0; i < ipt.frames.size() && i < 6; i++)
			printf("    %s\n", ipt.frames[i].c_str());
		if(ipt.frames.size() > 6)
			printf("    ... %d more\n", (int)(ipt.frames.size() - 6));
	}
	for(const std::string& note : ipt.notes)
		printf("  note      %s\n", note.c_str());
	return 0;
}

// ---------------------------------------------------------------------- dump
//
// One fact per line, nothing abbreviated: the acceptance check compares these
// against values counted by hand from the descriptor text.
static int Dump(const std::string& path)
{
	const std::vector<uint8_t> bytes = ReadFile(path);
	if(bytes.empty())
		return Fail("cannot read " + path);
	artemis::Ipt ipt;
	std::string error;
	if(!artemis::ParseIpt(std::string((const char*)bytes.data(), bytes.size()), ipt, error))
		return Fail(path + ": not an IPT descriptor: " + error);
	printf("file %s\n", path.c_str());
	printf("dialect %s\n", artemis::Name(ipt.dialect));
	if(!ipt.mode.empty())
		printf("mode %s\n", ipt.mode.c_str());
	if(ipt.dialect == artemis::IptDialect::Composite
		|| ipt.dialect == artemis::IptDialect::Anime || ipt.hasBase)
	{
		printf("canvas %d %d\n", ipt.width, ipt.height);
		printf("origin %d %d\n", ipt.baseX, ipt.baseY);
		printf("anchor %d %d\n", ipt.anchorX, ipt.anchorY);
	}
	if(!ipt.base.empty())
		printf("base %s\n", ipt.base.c_str());
	if(ipt.dialect == artemis::IptDialect::Composite)
	{
		printf("tiles %d\n", (int)ipt.tiles.size());
		for(const artemis::IptTile& tile : ipt.tiles)
			printf("tile %d id=%d file=%s x=%d y=%d\n", (int)tile.order, tile.id,
				tile.file.c_str(), tile.x, tile.y);
	}
	if(!ipt.rects.empty())
	{
		printf("rects %d\n", (int)ipt.rects.size());
		for(size_t i = 0; i < ipt.rects.size(); i++)
			printf("rect %d name=%s x=%d y=%d w=%d h=%d\n", (int)i,
				ipt.rects[i].name.c_str(), ipt.rects[i].x, ipt.rects[i].y,
				ipt.rects[i].w, ipt.rects[i].h);
	}
	if(!ipt.points.empty())
	{
		printf("points %d\n", (int)ipt.points.size());
		for(size_t i = 0; i < ipt.points.size(); i++)
			printf("point %d x=%d y=%d\n", (int)i, ipt.points[i].x, ipt.points[i].y);
	}
	if(!ipt.groups.empty())
	{
		printf("groups %d\n", (int)ipt.groups.size());
		for(size_t g = 0; g < ipt.groups.size(); g++)
		{
			const artemis::IptGroup& group = ipt.groups[g];
			printf("group %d name=%s entries=%d\n", (int)g, group.name.c_str(),
				(int)group.entries.size());
			for(const artemis::IptGroupEntry& entry : group.entries)
			{
				printf("group_entry %s %s", group.name.c_str(), entry.name.c_str());
				for(size_t v = 0; v < entry.values.size(); v++)
					printf(v == 0 ? " %d" : ",%d", entry.values[v]);
				printf("\n");
			}
		}
	}
	if(!ipt.frames.empty())
	{
		printf("frames %d\n", (int)ipt.frames.size());
		for(size_t i = 0; i < ipt.frames.size(); i++)
			printf("frame %d %s\n", (int)i, ipt.frames[i].c_str());
		printf("frame_time %d\n", ipt.frameTime);
		printf("frame_max %d\n", ipt.frameMax);
		printf("frame_loop %d\n", ipt.frameLoop);
	}
	for(const std::string& note : ipt.notes)
		printf("note %s\n", note.c_str());
	return 0;
}

// -------------------------------------------------------------------- decode

// Compose and write; missing tiles are fatal here because a game that ships a
// half image must surface that, not silently draw a hole.
static bool DecodeComposite(const artemis::Ipt& ipt, const std::string& directory,
	const artemis::ResourceReader& read, const std::string& outPath,
	artemis::ComposeReport& report, int& colors, std::string& error)
{
	unlink(outPath.c_str()); // never leave a previous run's image looking like success
	artemis::Image canvas;
	// Compose and encode are timed apart: on a real 1668x998 event CG the encode
	// is hundreds of milliseconds and the compose is single digits, so a single
	// number would blame the wrong half.
	const auto composeStart = std::chrono::steady_clock::now();
	if(!artemis::ComposeIpt(ipt, directory, read, canvas, report, error))
		return false;
	const long long composeUs = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - composeStart).count();
	const auto encodeStart = std::chrono::steady_clock::now();
	const auto encodeDone = [&] {
		return std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - encodeStart).count();
	};
	(void)encodeDone;
	if(!report.missing.empty())
	{
		error = std::to_string(report.missing.size()) + " referenced tile(s) are missing";
		for(const std::string& missing : report.missing)
			error += "\n  missing " + missing;
		return false;
	}
	if(canvas.hasAlpha)
	{
		if(!artemis::WritePng(outPath, canvas.width, canvas.height, canvas.rgba.data(),
			error))
			return false;
	}
	else
	{
		// a Bgr32 canvas (diff mode) has no alpha channel; write RGB, as the
		// reference does, so the output is unambiguously opaque
		std::vector<uint8_t> rgb((size_t)canvas.width * canvas.height * 3);
		for(size_t i = 0; i < canvas.pixels(); i++)
			for(int c = 0; c < 3; c++)
				rgb[i * 3 + c] = canvas.rgba[i * 4 + c];
		if(!artemis::WritePngRgb(outPath, canvas.width, canvas.height, rgb.data(), error))
			return false;
	}
	colors = (int)artemis::CountColors(canvas.rgba.data(), canvas.pixels());
	report.composeUs = composeUs;
	report.encodeUs = encodeDone();
	return true;
}

static int Decode(const artemis::Ipt& ipt, const std::string& directory,
	const artemis::ResourceReader& read, const std::string& outPath, bool quiet)
{
	artemis::ComposeReport report;
	int colors = 0;
	std::string error;
	if(!DecodeComposite(ipt, directory, read, outPath, report, colors, error))
		return Fail(error);
	printf("%s: %s, %d x %d, %d tile(s) drawn", outPath.c_str(), ipt.mode.c_str(),
		ipt.width, ipt.height, report.tilesDrawn);
	if(report.tilesSkipped)
		printf(", %d skipped", report.tilesSkipped);
	printf(", %d colour(s), %s\n", colors,
		ipt.mode == "diff" ? "opaque (Bgr32)" : "alpha (Bgra32)");
	// Self-reported split so a benchmark can tell the compositor from the encoder.
	printf("  apply %.3f ms  (compose + blend + clip)\n", report.composeUs / 1000.0);
	printf("  encode %.3f ms  (PNG write + colour count)\n", report.encodeUs / 1000.0);
	if(!quiet)
	{
		for(const std::string& note : report.notes)
			printf("  note      %s\n", note.c_str());
	}
	return 0;
}

static int DecodeFromDir(const std::string& descriptor, const std::string& outPath)
{
	const std::vector<uint8_t> bytes = ReadFile(descriptor);
	if(bytes.empty())
		return Fail("cannot read " + descriptor);
	artemis::Ipt ipt;
	std::string error;
	if(!artemis::ParseIpt(std::string((const char*)bytes.data(), bytes.size()), ipt, error))
		return Fail(descriptor + ": not an IPT descriptor: " + error);
	return Decode(ipt, "", DirReader(DirName(descriptor)), outPath, false);
}

static int DecodeFromArchive(const std::string& archivePath, const std::string& member,
	const std::string& outPath)
{
	try
	{
		artemis::Archive archive = artemis::Open(archivePath);
		const artemis::Entry* entry = artemis::Find(archive, member);
		if(entry == nullptr)
			return Fail("no member " + member + " in " + archivePath);
		const std::vector<uint8_t> bytes = artemis::Content(archive, *entry);
		artemis::Ipt ipt;
		std::string error;
		if(!artemis::ParseIpt(std::string((const char*)bytes.data(), bytes.size()), ipt, error))
			return Fail(member + ": not an IPT descriptor: " + error);
		const std::string directory = DirName(artemis::NormalizeName(member));
		printf("%s:%s (key phase %s)\n", archivePath.c_str(), member.c_str(),
			artemis::Name(archive.phase));
		return Decode(ipt, directory, ArchiveReader(archive), outPath, false);
	}
	catch(const std::exception& e)
	{
		return Fail(e.what());
	}
}

// ------------------------------------------------------------------ selftest

static int Check(bool ok, const char* what, int& failures)
{
	printf("  %-62s %s\n", what, ok ? "ok" : "FAILED");
	if(!ok)
		failures++;
	return ok ? 0 : 1;
}

static bool PixelIs(const artemis::Image& img, int x, int y, int r, int g, int b, int a)
{
	if(x < 0 || y < 0 || x >= img.width || y >= img.height)
		return false;
	const uint8_t* p = img.rgba.data() + ((size_t)y * img.width + x) * 4;
	return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

static bool AllPixelsOpaque(const artemis::Image& img)
{
	for(size_t i = 0; i < img.pixels(); i++)
		if(img.rgba[i * 4 + 3] != 0xFF)
			return false;
	return true;
}

static std::vector<uint8_t> SolidRgba(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	std::vector<uint8_t> out((size_t)w * h * 4);
	for(size_t i = 0; i < (size_t)w * h; i++)
	{
		out[i * 4 + 0] = r;
		out[i * 4 + 1] = g;
		out[i * 4 + 2] = b;
		out[i * 4 + 3] = a;
	}
	return out;
}

static std::vector<uint8_t> SolidRgb(int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
	std::vector<uint8_t> out((size_t)w * h * 3);
	for(size_t i = 0; i < (size_t)w * h; i++)
	{
		out[i * 3 + 0] = r;
		out[i * 3 + 1] = g;
		out[i * 3 + 2] = b;
	}
	return out;
}

static void WriteTile(const std::string& dir, const std::string& name, int w, int h,
	uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	std::string error;
	const std::vector<uint8_t> pixels = SolidRgba(w, h, r, g, b, a);
	artemis::WritePng(dir + "/" + name, w, h, pixels.data(), error);
}

static void WriteTileRgb(const std::string& dir, const std::string& name, int w, int h,
	uint8_t r, uint8_t g, uint8_t b)
{
	std::string error;
	// a PNG with no alpha channel at all: the compositor must copy it, not blend
	const std::vector<uint8_t> pixels = SolidRgb(w, h, r, g, b);
	artemis::WritePngRgb(dir + "/" + name, w, h, pixels.data(), error);
}

// A scratch directory that removes itself, however the test leaves.
struct TempDir
{
	std::string path;
	bool ok = false;
	TempDir()
	{
		char templ[] = "/tmp/iettool_XXXXXX";
		if(mkdtemp(templ) != nullptr)
		{
			path = templ;
			ok = true;
		}
	}
	~TempDir()
	{
		if(!ok)
			return;
		std::error_code ignored;
		std::filesystem::remove_all(path, ignored);
	}
	bool Write(const std::string& name, const std::string& text) const
	{
		return WriteText(path + "/" + name, text);
	}
};

static const artemis::IptRect* FindRect(const artemis::Ipt& ipt, const std::string& name)
{
	for(const artemis::IptRect& rect : ipt.rects)
		if(rect.name == name)
			return &rect;
	return nullptr;
}

static const artemis::IptGroup* FindGroup(const artemis::Ipt& ipt, const std::string& name)
{
	for(const artemis::IptGroup& group : ipt.groups)
		if(group.name == name)
			return &group;
	return nullptr;
}

static const artemis::IptGroupEntry* FindGroupEntry(const artemis::IptGroup& group,
	const std::string& name)
{
	for(const artemis::IptGroupEntry& entry : group.entries)
		if(entry.name == name)
			return &entry;
	return nullptr;
}

// The four raspberry descriptors: atlas, atlas, point table, animation.
static void TestRealRaspberry(int& failures)
{
	const std::string directory = SamplePath("artemis-raspberry-web/samples/ipt");
	const std::string dialog = directory + "/psw_ui_ja_mw_dialog.ipt";
	const std::string saveChar = directory + "/psw_ui_ja_save_char.ipt";
	const std::string map = directory + "/psw_ui_ja_mw_map.ipt";
	const std::string anime = directory + "/image_hd_anime_line1.ipt";
	if(!FileExists(dialog) || !FileExists(saveChar) || !FileExists(map) || !FileExists(anime))
	{
		printf("  (testdata/artemis-raspberry-web/samples/ipt is absent; skipped)\n");
		return;
	}

	std::string text;
	std::string error;
	artemis::Ipt ipt;

	// dialog: 17 named rectangles.  The file's eighteenth '=' is the outer
	// `ipt = {` assignment itself, not an entry.
	const bool dialogParsed = ReadText(dialog, text) && artemis::ParseIpt(text, ipt, error);
	Check(dialogParsed, "dialog.ipt parses", failures);
	if(dialogParsed)
	{
		Check(ipt.dialect == artemis::IptDialect::Atlas, "dialog.ipt is an atlas", failures);
		Check(ipt.rects.size() == 17, "dialog.ipt has 17 rects (not 18)", failures);
		const artemis::IptRect* first = FindRect(ipt, "ok_qs");
		const artemis::IptRect* load = FindRect(ipt, "load");
		const artemis::IptRect* last = FindRect(ipt, "favo");
		Check(first != nullptr && first->x == 576 && first->y == 0 && first->w == 424
			&& first->h == 36, "dialog.ipt ok_qs = 576,0,424,36", failures);
		Check(load != nullptr && load->x == 576 && load->y == 108,
			"dialog.ipt load = 576,108,...", failures);
		Check(last != nullptr && last->x == 0 && last->y == 320 && last->w == 424
			&& last->h == 36, "dialog.ipt last rect favo = 0,320,424,36", failures);
		bool allSized = ipt.rects.size() == 17;
		for(const artemis::IptRect& rect : ipt.rects)
			if(rect.w != 424 || rect.h != 36)
				allSized = false;
		Check(allSized, "every dialog.ipt rect is 424 x 36", failures);
		std::set<std::string> names;
		for(const artemis::IptRect& rect : ipt.rects)
			names.insert(rect.name);
		Check(names.size() == 17, "dialog.ipt has 17 distinct names", failures);
	}

	// save/char: four character thumbnails, bracketed Japanese keys.
	const bool saveParsed = ReadText(saveChar, text) && artemis::ParseIpt(text, ipt, error);
	Check(saveParsed, "save_char.ipt parses", failures);
	if(saveParsed)
	{
		Check(ipt.dialect == artemis::IptDialect::Atlas && ipt.rects.size() == 4,
			"save_char.ipt has 4 rects", failures);
		const artemis::IptRect* ruri = FindRect(ipt, "瑠莉");
		const artemis::IptRect* minato = FindRect(ipt, "みなと");
		Check(ruri != nullptr && ruri->x == 0 && ruri->y == 1 && ruri->w == 166
			&& ruri->h == 93, "save_char.ipt 瑠莉 = 0,1,166,93", failures);
		Check(minato != nullptr && minato->x == 0 && minato->y == 289,
			"save_char.ipt みなと = 0,289,...", failures);
		Check(FindRect(ipt, "美琴") != nullptr && FindRect(ipt, "悠") != nullptr,
			"save_char.ipt keeps every bracketed key", failures);
	}

	// map: eight hot spots plus trial and main, four characters each.
	const bool mapParsed = ReadText(map, text) && artemis::ParseIpt(text, ipt, error);
	Check(mapParsed, "map.ipt parses", failures);
	if(mapParsed)
	{
		Check(ipt.dialect == artemis::IptDialect::Data, "map.ipt is a point table", failures);
		Check(ipt.rects.empty(), "map.ipt has no rectangles", failures);
		Check(ipt.points.size() == 8, "map.ipt has 8 hot spots", failures);
		Check(ipt.points.size() == 8 && ipt.points[0].x == 50 && ipt.points[0].y == 394,
			"map.ipt point 1 = 50,394", failures);
		Check(ipt.points.size() == 8 && ipt.points[7].x == 225 && ipt.points[7].y == 101,
			"map.ipt point 8 = 225,101", failures);
		Check(ipt.groups.size() == 2, "map.ipt has trial and main", failures);
		const artemis::IptGroup* trial = FindGroup(ipt, "trial");
		const artemis::IptGroup* main = FindGroup(ipt, "main");
		Check(trial != nullptr && trial->entries.size() == 4,
			"trial has 4 characters", failures);
		Check(main != nullptr && main->entries.size() == 4,
			"main has 4 characters", failures);
		const artemis::IptGroupEntry* ruri = trial ? FindGroupEntry(*trial, "ruri") : nullptr;
		Check(ruri != nullptr && ruri->values.size() == 3 && ruri->values[0] == 1
			&& ruri->values[1] == 1 && ruri->values[2] == 5,
			"trial.ruri = 1,1,5", failures);
		const artemis::IptGroupEntry* mina = main ? FindGroupEntry(*main, "mina") : nullptr;
		Check(mina != nullptr && mina->values.size() == 3 && mina->values[2] == 8,
			"main.mina = 8,8,8", failures);
		const artemis::IptGroupEntry* yuu = main ? FindGroupEntry(*main, "yuu_") : nullptr;
		Check(yuu != nullptr && yuu->values.size() == 3 && yuu->values[0] == 4,
			"main.yuu_ = 4,7,3", failures);
	}

	// line1: an anime_full sequence with three frames.
	const bool animeParsed = ReadText(anime, text) && artemis::ParseIpt(text, ipt, error);
	Check(animeParsed, "line1.ipt parses", failures);
	if(animeParsed)
	{
		Check(ipt.dialect == artemis::IptDialect::Anime, "line1.ipt is an animation", failures);
		Check(ipt.mode == "anime_full", "line1.ipt mode anime_full", failures);
		Check(ipt.width == 1280 && ipt.height == 720, "line1.ipt canvas 1280 x 720", failures);
		Check(ipt.frames.size() == 3, "line1.ipt has 3 frames", failures);
		Check(ipt.frames.size() == 3 && ipt.frames[0] == "line11"
			&& ipt.frames[2] == "line13", "line1.ipt frames line11..line13", failures);
		Check(ipt.frameTime == 100 && ipt.frameMax == 3 && ipt.frameLoop == -1,
			"line1.ipt time/max/loop = 100/3/-1", failures);
	}
}

// Every henpri composite descriptor, structure only.  The descriptors are text;
// no image member of that archive is read here.
static void TestRealComposites(int& failures)
{
	const std::string directory = SamplePath("artemis-henpri-trial/extracted/ipt_descriptors");
	const std::string ev = directory + "/ev_chi_01/ev_chi_01_02.ipt";
	const std::string zev = directory + "/ev_himh_01/zev_himh_01_02.ipt";
	if(!FileExists(ev) || !FileExists(zev))
	{
		printf("  (testdata/artemis-henpri-trial/extracted/ipt_descriptors is absent; skipped)\n");
		return;
	}

	std::string text;
	std::string error;
	artemis::Ipt ipt;

	const bool evParsed = ReadText(ev, text) && artemis::ParseIpt(text, ipt, error);
	Check(evParsed, "ev_chi_01_02.ipt parses", failures);
	if(evParsed)
	{
		Check(ipt.dialect == artemis::IptDialect::Composite, "it is composite", failures);
		Check(ipt.mode == "diff", "it is diff mode", failures);
		Check(ipt.width == 1668 && ipt.height == 998, "canvas is 1668 x 998", failures);
		Check(ipt.baseX == -34 && ipt.baseY == -49, "image origin is -34,-49", failures);
		Check(ipt.anchorX == 834 && ipt.anchorY == 499, "anchor is 834,499", failures);
		Check(ipt.base == "ev_chi_01_01", "base is ev_chi_01_01", failures);
		Check(ipt.tiles.size() == 1, "one tile", failures);
		Check(ipt.tiles.size() == 1 && ipt.tiles[0].file == "ev_chi_01_02"
			&& ipt.tiles[0].x == 773 && ipt.tiles[0].y == 244,
			"tile ev_chi_01_02 at 773,244", failures);
		Check(ipt.tiles.size() == 1 && ipt.tiles[0].id == 1 && ipt.tiles[0].hasId,
			"tile id 1 came from the descriptor", failures);
	}

	const bool zevParsed = ReadText(zev, text) && artemis::ParseIpt(text, ipt, error);
	Check(zevParsed, "zev_himh_01_02.ipt parses", failures);
	if(zevParsed)
	{
		Check(ipt.width == 3000 && ipt.height == 2000, "zev canvas is 3000 x 2000", failures);
		Check(ipt.baseX == -700 && ipt.baseY == -550, "zev origin is -700,-550", failures);
		Check(ipt.base == "zev_himh_01_01", "zev base is zev_himh_01_01", failures);
		Check(ipt.tiles.size() == 1 && ipt.tiles[0].x == 441 && ipt.tiles[0].y == 470,
			"zev tile at 441,470", failures);
	}

	// Walk all 66: each is a one-tile diff whose tile is named after its file.
	int parsed = 0;
	int faithful = 0;
	int tileTotal = 0;
	std::set<std::string> bases;
	std::vector<std::string> problems;
	std::error_code ec;
	for(const std::string& group : { std::string("ev_chi_01"), std::string("ev_himh_01") })
	{
		for(std::filesystem::directory_iterator it(directory + "/" + group, ec), end;
			!ec && it != end; it.increment(ec))
		{
			if(!it->is_regular_file() || it->path().extension() != ".ipt")
				continue;
			parsed++;
			if(!ReadText(it->path().string(), text))
			{
				problems.push_back(it->path().string() + ": unreadable");
				continue;
			}
			if(!artemis::ParseIpt(text, ipt, error))
			{
				problems.push_back(it->path().filename().string() + ": " + error);
				continue;
			}
			const std::string stem = it->path().stem().string();
			const bool good = ipt.dialect == artemis::IptDialect::Composite
				&& ipt.mode == "diff" && ipt.width > 0 && ipt.height > 0
				&& !ipt.base.empty() && ipt.tiles.size() == 1
				&& ipt.tiles[0].file == stem && ipt.tiles[0].id == 1
				&& ipt.tiles[0].hasId
				&& ipt.tiles[0].x >= 0 && ipt.tiles[0].x < ipt.width
				&& ipt.tiles[0].y >= 0 && ipt.tiles[0].y < ipt.height;
			if(good)
				faithful++;
			else
				problems.push_back(it->path().filename().string() + ": unexpected shape");
			tileTotal += (int)ipt.tiles.size();
			bases.insert(ipt.base);
		}
	}
	Check(parsed == 66, "66 real composite descriptors found", failures);
	Check(faithful == 66, "all 66 are one-tile diff composites named after the file", failures);
	Check(tileTotal == 66, "66 tiles across the corpus", failures);
	Check(bases.size() == 4, "four base images (ev/zev chi/himh) across the corpus", failures);
	for(size_t i = 0; i < problems.size() && i < 3; i++)
		printf("      %s\n", problems[i].c_str());
}

// The real descriptor's own fields drive a pixel test whose tiles are ours.
static void TestRealDescriptorPixels(int& failures)
{
	const std::string descriptorPath =
		SamplePath("artemis-henpri-trial/extracted/ipt_descriptors/ev_chi_01/ev_chi_01_02.ipt");
	if(!FileExists(descriptorPath))
	{
		printf("  (the real ev_chi_01_02.ipt is absent; skipped)\n");
		return;
	}
	std::string text;
	if(!ReadText(descriptorPath, text))
	{
		Check(false, "the real descriptor is readable", failures);
		return;
	}
	TempDir dir;
	if(!dir.ok)
	{
		Check(false, "a temp directory for the pixel test", failures);
		return;
	}
	// tiles built to the names the descriptor asks for; the canvas is the base's
	// 1668 x 998 from the descriptor itself
	WriteTile(dir.path, "ev_chi_01_01.png", 1668, 998, 255, 255, 255, 255);
	WriteTile(dir.path, "ev_chi_01_02.png", 100, 80, 10, 200, 30, 128);
	if(!dir.Write("ev_chi_01_02.ipt", text))
	{
		Check(false, "the real descriptor text is written beside the tiles", failures);
		return;
	}
	const int rc = DecodeFromDir(dir.path + "/ev_chi_01_02.ipt", dir.path + "/out.png");
	Check(rc == 0, "decode with the real descriptor and our tiles", failures);
	artemis::Image got;
	std::string error;
	const bool readBack = artemis::ReadPng(dir.path + "/out.png", got, error);
	Check(readBack, "the decoded PNG reads back", failures);
	if(!readBack)
		return;
	Check(got.width == 1668 && got.height == 998, "decoded canvas is 1668 x 998", failures);
	Check(!got.hasAlpha, "diff output is RGB (Bgr32), no alpha channel", failures);
	Check(PixelIs(got, 0, 0, 255, 255, 255, 255), "base laid down at 0,0", failures);
	// (10,200,30,128) over white, per-channel: 132, 227, 142
	Check(PixelIs(got, 773, 244, 132, 227, 142, 255),
		"tile blends at 773,244 -> 132,227,142", failures);
	Check(PixelIs(got, 872, 323, 132, 227, 142, 255),
		"the tile's far corner at 872,323 blends too", failures);
	Check(PixelIs(got, 772, 244, 255, 255, 255, 255), "one pixel left of the tile stays base", failures);
	Check(PixelIs(got, 873, 323, 255, 255, 255, 255), "one pixel right of the tile stays base", failures);
	Check(AllPixelsOpaque(got), "every pixel of the diff result is opaque", failures);
	const size_t colors = artemis::CountColors(got.rgba.data(), got.pixels());
	Check(colors == 2, "exactly two colours (white + blend)", failures);
}

// The five compositing semantics, on fixtures whose pixels are all ours.
static void TestCompositeFixtures(int& failures)
{
	std::string error;
	artemis::Ipt ipt;

	printf("cut mode: copy, blend, clip and draw order\n");
	{
		TempDir dir;
		Check(dir.ok, "temp dir", failures);
		if(dir.ok)
		{
			WriteTile(dir.path, "a.png", 4, 4, 200, 0, 0, 255);       // opaque red
			WriteTile(dir.path, "b.png", 4, 4, 0, 0, 200, 128);       // half blue
			WriteTileRgb(dir.path, "c.png", 2, 2, 0, 255, 0);         // green, no alpha
			WriteTile(dir.path, "d.png", 3, 3, 255, 0, 255, 255);     // clipped at the edge
			WriteTile(dir.path, "off.png", 4, 4, 255, 255, 0, 255);   // drawn at -2,-2
			// ids deliberately out of order: the array order is the draw order
			const std::string descriptor =
				"ipt = {\n"
				"\tmode = \"cut\",\n"
				"\tbase = { \"unused\", w = 8, h = 6, x = 100, y = 200 },\n"
				"\t{ id = 9, file = \"a\",   x = 0, y = 0 },\n"
				"\t{ id = 1, file = \"b\",   x = 2, y = 2 },\n"
				"\t{ id = 2, file = \"c\",   x = 6, y = 0 },\n"
				"\t{ id = 3, file = \"d.png\", x = 7, y = 5 },\n"
				"\t{ id = 0, file = \"off\", x = -2, y = -2 },\n"
				"}\n";
			Check(dir.Write("cut.ipt", descriptor), "descriptor written", failures);
			Check(artemis::ParseIpt(descriptor, ipt, error), "cut fixture parses", failures);
			Check(ipt.tiles.size() == 5 && ipt.tiles[3].file == "d.png",
				"all five tiles, the .png name kept as written", failures);
			const int rc = DecodeFromDir(dir.path + "/cut.ipt", dir.path + "/cut.png");
			Check(rc == 0, "cut decode wrote a PNG", failures);
			artemis::Image got;
			const bool cutRead = artemis::ReadPng(dir.path + "/cut.png", got, error);
			Check(cutRead, "the cut PNG reads back", failures);
			if(cutRead)
			{
				Check(got.width == 8 && got.height == 6, "cut canvas is 8 x 6", failures);
				Check(got.hasAlpha, "cut output keeps an alpha channel (Bgra32)", failures);
				Check(PixelIs(got, 0, 2, 200, 0, 0, 255), "red copied where nothing covers it", failures);
				Check(PixelIs(got, 2, 2, 99, 0, 100, 128),
					"half-alpha tile blends over red (99,0,100,128)", failures);
				Check(PixelIs(got, 5, 5, 0, 0, 200, 128),
					"half-alpha tile over empty is copied (0,0,200,128)", failures);
				Check(PixelIs(got, 6, 0, 0, 255, 0, 255), "a tile with no alpha is copied opaquely", failures);
				Check(PixelIs(got, 7, 1, 0, 255, 0, 255), "the no-alpha tile covers both columns", failures);
				Check(PixelIs(got, 7, 5, 255, 0, 255, 255),
					"a tile at the bottom-right is clipped, its one visible pixel drawn", failures);
				Check(PixelIs(got, 6, 5, 0, 0, 0, 0), "the clipped-away part stays transparent", failures);
				Check(PixelIs(got, 0, 0, 255, 255, 0, 255),
					"the last tile in the file wins at 0,0 despite id 0", failures);
				Check(PixelIs(got, 1, 1, 255, 255, 0, 255),
					"a negative origin is clipped, not shifted", failures);
				Check(PixelIs(got, 2, 0, 200, 0, 0, 255),
					"horizontal clipping stops the tile at x=2 (no spill)", failures);
				Check(PixelIs(got, 3, 1, 200, 0, 0, 255),
					"column 3 keeps the tile underneath", failures);
				Check(PixelIs(got, 1, 2, 200, 0, 0, 255),
					"the clipped tile stops at its 4x4 edge", failures);
			}
		}
	}

	printf("diff mode: base first, Bgr32, blend keeps opacity\n");
	{
		TempDir dir;
		Check(dir.ok, "temp dir", failures);
		if(dir.ok)
		{
			WriteTile(dir.path, "bg.png", 8, 6, 255, 255, 255, 255);
			WriteTile(dir.path, "d.png", 2, 2, 0, 0, 0, 128);
			// "gone" is referenced by the descriptor and deliberately not written
			const std::string descriptor =
				"ipt = { mode = \"diff\", "
				"base = { \"bg\", w = 8, h = 6 }, "
				"{ id = 0, file = \"d\", x = 1, y = 1 }, "
				"{ id = 1, file = \"gone\", x = 4, y = 4 } }\n";
			Check(artemis::ParseIpt(descriptor, ipt, error), "diff fixture parses", failures);
			artemis::Image canvas;
			artemis::ComposeReport report;
			const bool composed = artemis::ComposeIpt(ipt, "", DirReader(dir.path), canvas,
				report, error);
			Check(composed, "diff composes with one absent tile", failures);
			if(composed)
			{
				Check(!canvas.hasAlpha, "diff canvas is Bgr32 (hasAlpha false)", failures);
				Check(PixelIs(canvas, 1, 1, 127, 127, 127, 255),
					"half-alpha over white blends to 127 and stays opaque", failures);
				Check(PixelIs(canvas, 0, 0, 255, 255, 255, 255), "base opaque outside the tile", failures);
				Check(AllPixelsOpaque(canvas), "no pixel of a diff canvas is translucent", failures);
				Check(report.tilesSkipped == 1 && report.missing.size() == 1,
					"the absent tile is reported, not fatal at compose level", failures);
			}
			// the CLI turns that report into a failure, and leaves no output behind
			dir.Write("missing.ipt", descriptor);
			const int rc = DecodeFromDir(dir.path + "/missing.ipt", dir.path + "/missing.png");
			Check(rc == 1, "decode refuses a descriptor with a missing tile", failures);
			Check(!FileExists(dir.path + "/missing.png"), "no half image is written", failures);
			// and the success path writes an RGB PNG with no alpha channel
			WriteTile(dir.path, "one.png", 2, 2, 0, 0, 0, 128);
			const std::string okDescriptor =
				"ipt = { mode = \"diff\", base = { \"bg\", w = 8, h = 6 }, "
				"{ id = 0, file = \"one\", x = 1, y = 1 } }\n";
			dir.Write("ok.ipt", okDescriptor);
			const int okRc = DecodeFromDir(dir.path + "/ok.ipt", dir.path + "/ok.png");
			Check(okRc == 0, "diff decode with every tile present succeeds", failures);
			artemis::Image got;
			Check(artemis::ReadPng(dir.path + "/ok.png", got, error)
				&& !got.hasAlpha && PixelIs(got, 1, 1, 127, 127, 127, 255),
				"the written diff PNG is RGB and opaque", failures);
		}
	}

	printf("diff mode: the base is drawn at 0,0, not at base.x/base.y\n");
	{
		TempDir dir;
		Check(dir.ok, "temp dir", failures);
		if(dir.ok)
		{
			WriteTile(dir.path, "small.png", 4, 4, 255, 255, 255, 255);
			// x=3,y=3 is the image's world offset; inside the canvas the base
			// still starts at 0,0 (reference/ImageIPT.cs:112)
			const std::string descriptor =
				"ipt = { mode = \"diff\", base = { \"small\", x = 3, y = 3, w = 8, h = 6 } }\n";
			Check(artemis::ParseIpt(descriptor, ipt, error), "offset-base fixture parses", failures);
			artemis::Image canvas;
			artemis::ComposeReport report;
			const bool composed = artemis::ComposeIpt(ipt, "", DirReader(dir.path), canvas,
				report, error);
			Check(composed, "offset-base fixture composes", failures);
			if(composed)
			{
				Check(PixelIs(canvas, 0, 0, 255, 255, 255, 255),
					"base pixel lands at 0,0 even though base.x/y are 3,3", failures);
				Check(PixelIs(canvas, 3, 3, 255, 255, 255, 255), "the 4x4 base covers 3,3", failures);
				Check(PixelIs(canvas, 4, 4, 0, 0, 0, 255),
					"outside the base a Bgr32 canvas is opaque black", failures);
			}
		}
	}
}

static void TestRejections(int& failures)
{
	std::string error;
	artemis::Ipt ipt;

	printf("refusing what is not a composite descriptor\n");
	Check(!artemis::ParseIpt("astver = 2.0\nast = { block_00000 = { } }\n", ipt, error)
		&& error.find("no 'ipt' table") != std::string::npos,
		"a .ast-shaped Lua table is refused by name", failures);
	Check(!artemis::ParseIpt("this is not a descriptor at all\n", ipt, error),
		"free text is refused", failures);
	Check(!artemis::ParseIpt("ipt = { mode = \"cut\", base = { } }\n", ipt, error),
		"a cut canvas without w/h is refused", failures);
	Check(!artemis::ParseIpt("ipt = { mode = \"cut\", base = { w = 8, h = 6 } }\n", ipt, error),
		"cut mode with no tiles is refused", failures);
	Check(!artemis::ParseIpt("ipt = { mode = \"diff\", base = { w = 8, h = 6 } }\n", ipt, error),
		"diff mode with no base name is refused", failures);
	Check(!artemis::ParseIpt(
		"ipt = { mode = \"cut\", base = { w = 100000, h = 100000 }, "
		"{ id = 0, file = \"a\", x = 0, y = 0 } }\n", ipt, error)
		&& error.find("too large") != std::string::npos,
		"an absurd canvas is refused instead of allocated", failures);

	// a present tile that is not a PNG: a hard error everywhere
	TempDir dir;
	Check(dir.ok, "temp dir for the non-PNG tile", failures);
	if(dir.ok)
	{
		dir.Write("bad.png", "this is not a PNG");
		const std::string descriptor =
			"ipt = { mode = \"cut\", base = { w = 4, h = 4 }, "
			"{ id = 0, file = \"bad\", x = 0, y = 0 } }\n";
		Check(artemis::ParseIpt(descriptor, ipt, error), "non-PNG fixture parses", failures);
		artemis::Image canvas;
		artemis::ComposeReport report;
		const bool composed = artemis::ComposeIpt(ipt, "", DirReader(dir.path), canvas,
			report, error);
		Check(!composed && error.find("not a readable PNG") != std::string::npos,
			"a tile that is not a PNG is a hard error", failures);
		dir.Write("bad.ipt", descriptor);
		const int rc = DecodeFromDir(dir.path + "/bad.ipt", dir.path + "/bad-out.png");
		Check(rc == 1, "the CLI exits non-zero for a non-PNG tile", failures);
		Check(!FileExists(dir.path + "/bad-out.png"), "no output is left behind", failures);
	}

	// `info`/`dump` on a real PNG must say "not an IPT descriptor", not crash
	TempDir imageDir;
	Check(imageDir.ok, "temp dir for the non-IPT file", failures);
	if(imageDir.ok)
	{
		WriteTile(imageDir.path, "page.png", 4, 4, 1, 2, 3, 255);
		const std::string png = imageDir.path + "/page.png";
		Check(Info(png) == 1, "info rejects a PNG with a non-zero status", failures);
		Check(Dump(png) == 1, "dump rejects a PNG with a non-zero status", failures);
		std::string pngText;
		Check(ReadText(png, pngText) && !artemis::ParseIpt(pngText, ipt, error)
			&& error.find("binary") != std::string::npos,
			"a PNG is named as binary, not as a broken Lua table", failures);
		Check(!artemis::ParseIpt("ipt = 3\n", ipt, error), "an ipt that is not a table is refused", failures);
	}
}

static void TestArchive(int& failures)
{
	printf("tiles inside an archive, decoded without extracting\n");
	TempDir dir;
	Check(dir.ok, "temp dir for the archives", failures);
	if(!dir.ok)
		return;

	std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
	std::string pngError;
	std::vector<uint8_t> tile;
	const std::vector<uint8_t> red = SolidRgba(4, 4, 200, 0, 0, 255);
	artemis::WritePngToMemory(tile, 4, 4, red.data(), pngError);
	files.emplace_back("root\\image\\ev\\ev01\\a.png", tile);
	const std::string descriptor =
		"ipt = { mode = \"cut\", base = { \"none\", w = 4, h = 4 }, "
		"{ id = 0, file = \"a\", x = 0, y = 0 } }\n";
	files.emplace_back("root\\image\\ev\\ev01\\x.ipt",
		std::vector<uint8_t>(descriptor.begin(), descriptor.end()));

	const std::string outPath = dir.path + "/archive-out.png";
	for(const std::string& magic : { std::string("pf8"), std::string("pf6") })
	{
		const bool encrypt = magic == "pf8";
		const std::vector<uint8_t> archive = artemis::Build(files, magic, encrypt,
			artemis::KeyPhase::EntryRelative);
		const std::string archivePath = dir.path + "/root." + magic + ".pfs";
		if(!WriteFile(archivePath, archive))
		{
			Check(false, "the archive was written", failures);
			continue;
		}
		const std::string what = magic + " archive decode wrote a PNG";
		const int rc = DecodeFromArchive(archivePath, "root/image/ev/ev01/x.ipt", outPath);
		Check(rc == 0, what.c_str(), failures);
		artemis::Image got;
		std::string error;
		Check(artemis::ReadPng(outPath, got, error)
			&& PixelIs(got, 0, 0, 200, 0, 0, 255)
			&& PixelIs(got, 3, 3, 200, 0, 0, 255),
			magic == "pf8" ? "the pf8 tile came through the backslash path"
				: "the pf6 tile came through the backslash path", failures);
		unlink(outPath.c_str());
	}

	// the same archive without its tile: a clear error, not a crash and not a
	// transparent PNG pretending to be the image
	std::vector<std::pair<std::string, std::vector<uint8_t>>> empty = {
		{ "root\\image\\ev\\ev01\\x.ipt",
			std::vector<uint8_t>(descriptor.begin(), descriptor.end()) } };
	const std::vector<uint8_t> broken = artemis::Build(empty, "pf8", true,
		artemis::KeyPhase::EntryRelative);
	const std::string brokenPath = dir.path + "/broken.pfs.002";
	Check(WriteFile(brokenPath, broken), "the tile-less archive was written", failures);
	const int missingRc = DecodeFromArchive(brokenPath, "root/image/ev/ev01/x.ipt", outPath);
	Check(missingRc == 1, "decode from an archive with a missing tile fails", failures);
	Check(!FileExists(outPath), "no output image for the broken archive", failures);

	// and the error names the member, so a player sees what is wrong
	try
	{
		artemis::Archive archive = artemis::Open(brokenPath);
		const artemis::Entry* entry = artemis::Find(archive, "root/image/ev/ev01/x.ipt");
		artemis::Ipt ipt;
		std::string error;
		artemis::Image canvas;
		artemis::ComposeReport report;
		bool parsed = false;
		if(entry != nullptr)
		{
			const std::vector<uint8_t> bytes = artemis::Content(archive, *entry);
			parsed = artemis::ParseIpt(std::string((const char*)bytes.data(), bytes.size()),
				ipt, error);
		}
		const bool composed = parsed && artemis::ComposeIpt(ipt,
			DirName(artemis::NormalizeName("root/image/ev/ev01/x.ipt")),
			ArchiveReader(archive), canvas, report, error);
		Check(composed && report.missing.size() == 1
			&& report.missing[0].find("ev01/a.png") != std::string::npos,
			"the missing report names image/ev/ev01/a.png", failures);
	}
	catch(const std::exception& e)
	{
		printf("      %s\n", e.what());
		Check(false, "the broken archive still opens", failures);
	}
}

// Structure-only check of the real 211 MB archive: the descriptor is read, no
// image member is touched.
static void TestRealArchive(int& failures)
{
	const std::string path = SamplePath("artemis-henpri-trial/raw/HENPRI.pfs.060");
	if(!FileExists(path))
	{
		printf("  (testdata/artemis-henpri-trial/raw/HENPRI.pfs.060 is absent; skipped)\n");
		return;
	}
	try
	{
		artemis::Archive archive = artemis::Open(path);
		Check(archive.streaming(), "the 211 MB archive is mapped, not loaded", failures);
		const artemis::Entry* entry = artemis::Find(archive, "image\\ev\\ev_chi_01\\ev_chi_01_02.ipt");
		Check(entry != nullptr, "the real composite descriptor is in the archive", failures);
		if(entry == nullptr)
			return;
		const std::vector<uint8_t> bytes = artemis::Content(archive, *entry);
		artemis::Ipt ipt;
		std::string error;
		Check(artemis::ParseIpt(std::string((const char*)bytes.data(), bytes.size()), ipt, error)
			&& ipt.dialect == artemis::IptDialect::Composite
			&& ipt.width == 1668 && ipt.height == 998 && ipt.tiles.size() == 1
			&& ipt.tiles[0].file == "ev_chi_01_02" && ipt.tiles[0].x == 773
			&& ipt.tiles[0].y == 244,
			"it parses to the same canvas and tile as the extracted copy", failures);
		// presence is a property of the archive build, not of the parser: report
		// it, do not assert it, and never read the image bytes either way.
		const artemis::Entry* base = artemis::Find(archive, "image\\ev\\ev_chi_01\\ev_chi_01_01.png");
		printf("      archive build: base tile member %s (payload not read)\n",
			base != nullptr ? "present" : "absent");
	}
	catch(const std::exception& e)
	{
		printf("      %s\n", e.what());
		Check(false, "the real archive opens", failures);
	}
}

static int SelfTest()
{
	int failures = 0;

	printf("the blend rule, on its own\n");
	{
		uint8_t dst[4] = { 200, 0, 0, 255 };
		const uint8_t src[4] = { 0, 0, 200, 128 };
		artemis::BlendPixel(dst, src);
		// (0*128 + 200*127)/255 = 99, (200*128 + 0*127)/255 = 100
		Check(dst[0] == 99 && dst[1] == 0 && dst[2] == 100 && dst[3] == 128,
			"  half-alpha over opaque blends per channel", failures);

		uint8_t transparent[4] = { 0, 0, 0, 0 };
		const uint8_t opaque[4] = { 10, 20, 30, 255 };
		artemis::BlendPixel(transparent, opaque);
		Check(memcmp(transparent, opaque, 4) == 0, "  opaque over empty copies", failures);

		uint8_t untouched[4] = { 1, 2, 3, 4 };
		const uint8_t invisible[4] = { 9, 9, 9, 0 };
		artemis::BlendPixel(untouched, invisible);
		Check(untouched[0] == 1 && untouched[3] == 4, "  alpha 0 changes nothing", failures);
	}

	printf("the descriptor text, as the real files write it\n");
	{
		artemis::Ipt ipt;
		std::string error;
		const std::string atlas =
			"ipt = {\n"
			"\tok_qs = \"576,0,424,36\",\n"
			"\t[\"瑠莉\"]\t= \"0,1,166,93\",\n"
			"\t[\"美琴\"]\t= \"0,97,166,93\",\n"
			"}\n";
		Check(artemis::ParseIpt(atlas, ipt, error)
			&& ipt.dialect == artemis::IptDialect::Atlas && ipt.rects.size() == 3,
			"  atlas dialect with bracketed Japanese keys", failures);
		if(ipt.dialect == artemis::IptDialect::Atlas && ipt.rects.size() == 3)
			Check(ipt.rects[2].name == "美琴" && ipt.rects[1].w == 166,
				"  atlas rectangle came out of the string", failures);

		const std::string data =
			"ipt = {\n"
			"\t{ x=50 , y=394 },\t\t-- hot spot with a Japanese comment\n"
			"\t{ x=225, y=489 },\n"
			"\tmain = { ruri = { 1, 1, 5 } },\n"
			"}\n";
		Check(artemis::ParseIpt(data, ipt, error)
			&& ipt.dialect == artemis::IptDialect::Data && ipt.points.size() == 2
			&& ipt.groups.size() == 1 && ipt.groups[0].entries.size() == 1
			&& ipt.groups[0].entries[0].values.size() == 3
			&& ipt.groups[0].entries[0].values[2] == 5,
			"  data dialect with points and nested groups", failures);

		const std::string anime =
			"ipt = {\n"
			"\tmode = \"anime_full\",\n"
			"\tbase = { x=0, y=0, w=1280, h=720, ax=640, ay=360, time=100, max=3, loop=-1 },\n"
			"\t{\"line11\"},\n"
			"\t{\"line12\"},\n"
			"\t{\"line13\"},\n"
			"}\n";
		Check(artemis::ParseIpt(anime, ipt, error)
			&& ipt.dialect == artemis::IptDialect::Anime && ipt.frames.size() == 3
			&& ipt.frameTime == 100 && ipt.frameMax == 3 && ipt.frameLoop == -1,
			"  anime dialect with three frames and timing", failures);

		artemis::Ipt notanimage;
		artemis::Image dummy;
		artemis::ComposeReport dummyReport;
		std::string composeError;
		const bool parsedAtlas = artemis::ParseIpt(atlas, notanimage, error);
		const bool composedAtlas = parsedAtlas && artemis::ComposeIpt(notanimage, "",
			DirReader("."), dummy, dummyReport, composeError);
		Check(parsedAtlas && !composedAtlas,
			"  composing a non-composite dialect is refused", failures);
	}

	printf("the real raspberry descriptors (structure only)\n");
	TestRealRaspberry(failures);

	printf("the 66 real henpri composite descriptors (structure only)\n");
	TestRealComposites(failures);

	printf("a real descriptor driving our own pixels\n");
	TestRealDescriptorPixels(failures);

	TestCompositeFixtures(failures);
	TestRejections(failures);
	TestArchive(failures);

	printf("the real archive (structure only, no image payload read)\n");
	TestRealArchive(failures);

	printf(failures == 0 ? "all passed\n" : "%d FAILED\n", failures);
	return failures == 0 ? 0 : 1;
}

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		fprintf(stderr,
			"usage: %s info <descriptor.ipt>\n"
			"       %s dump <descriptor.ipt>\n"
			"       %s decode <descriptor.ipt> <out.png>\n"
			"       %s decode --archive <archive.pfs> <member.ipt> <out.png>\n"
			"       %s selftest\n", argv[0], argv[0], argv[0], argv[0], argv[0]);
		return 2;
	}
	const std::string cmd(argv[1]);
	if(cmd == "info" && argc >= 3)
		return Info(argv[2]);
	if(cmd == "dump" && argc >= 3)
		return Dump(argv[2]);
	if(cmd == "decode" && argc >= 4)
	{
		if(std::string(argv[2]) == "--archive")
		{
			if(argc < 6)
				return Fail("decode --archive needs <archive> <member> <out.png>");
			return DecodeFromArchive(argv[3], argv[4], argv[5]);
		}
		return DecodeFromDir(argv[2], argv[3]);
	}
	if(cmd == "selftest")
		return SelfTest();
	fprintf(stderr, "iettool: bad arguments\n");
	return 2;
}
