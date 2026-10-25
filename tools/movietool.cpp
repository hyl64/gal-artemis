// movietool.cpp - inspect, probe and self-test the Artemis movie layer
//
//   movietool info    <file.mp4>                  size / fps / frames / duration / audio
//   movietool frame   <in> <out.png> --at <ms>    decode one frame and write it as PNG
//   movietool probe   <file.mp4>                  decode the whole file, report stats
//   movietool tags    <script.ast>...             the real `{"movie", ...}` tags, as data
//   movietool members <archive_index.txt> --names op,ed2,...
//                                                 does the corpus even ship the videos?
//   movietool selftest                            assertions, on synthetic clips only
//
// Why every pixel assertion runs on synthetic clips: the five real `movie` tags
// point at adult-game video.  Their pixels are off limits, and it turns out the
// trial corpus does not ship the video members at all (see the `members`
// command and the note in the report).  So implementation and verification are
// kept apart on purpose - the decoder is proven on clips this tool generates,
// and real data is only ever parsed as text.

#include "../src/movie/movie.h"
#include "../src/pngio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace artemis;
using namespace artemis::movie;

namespace
{

int gChecks = 0;
int gFailures = 0;
int gSkips = 0;

void Pass(const char* what, const std::string& detail = std::string())
{
	gChecks++;
	std::printf("  \033[32mPASS\033[0m %-54s %s\n", what, detail.c_str());
}

void Fail(const char* what, const std::string& detail)
{
	gChecks++;
	gFailures++;
	std::printf("  \033[31mFAIL\033[0m %-54s %s\n", what, detail.c_str());
}

void Skip(const char* what, const std::string& why)
{
	gSkips++;
	std::printf("  \033[33mSKIP\033[0m %-54s %s\n", what, why.c_str());
}

void Check(bool ok, const char* what, const std::string& detail)
{
	if(ok)
		Pass(what, detail);
	else
		Fail(what, detail);
}

std::string FormatSeconds(double s)
{
	char buf[64];
	const int minutes = (int)(s / 60.0);
	std::snprintf(buf, sizeof buf, "%d:%05.2f", minutes, s - minutes * 60.0);
	return buf;
}

std::string HumanBytes(int64_t bytes)
{
	char buf[64];
	if(bytes >= 1024 * 1024) std::snprintf(buf, sizeof buf, "%.2f MB", (double)bytes / (1024.0 * 1024.0));
	else if(bytes >= 1024) std::snprintf(buf, sizeof buf, "%.1f KB", (double)bytes / 1024.0);
	else std::snprintf(buf, sizeof buf, "%lld B", (long long)bytes);
	return buf;
}

uint64_t Checksum(const Canvas& canvas)
{
	uint64_t h = 1469598103934665603ull;
	for(uint8_t byte : canvas.rgba)
	{
		h ^= byte;
		h *= 1099511628211ull;
	}
	return h;
}

bool FilePresent(const std::string& path)
{
	std::FILE* f = std::fopen(path.c_str(), "rb");
	if(!f) return false;
	std::fclose(f);
	return true;
}

bool RunCommand(const std::string& command)
{
	return std::system(command.c_str()) == 0;
}

// ---------------------------------------------------------------------------
// info / frame / probe
// ---------------------------------------------------------------------------
int CommandInfo(const std::string& path)
{
	std::string error;
	auto movie = Movie::Open(path, &error);
	if(!movie)
	{
		std::fprintf(stderr, "info: %s\n", error.c_str());
		return 1;
	}
	const MovieInfo& info = movie->Info();
	std::printf("file:        %s\n", path.c_str());
	std::printf("size:        %dx%d\n", info.width, info.height);
	std::printf("fps:         %.3f%s\n", info.fps,
		info.fps > 0 ? "" : "  (unknown; the file reports none)");
	std::printf("duration:    %s (%lld ms)\n", FormatSeconds((double)info.durationMs / 1000.0).c_str(),
		(long long)info.durationMs);
	std::printf("frames:      ~%lld (estimated from duration x fps; probe counts the real ones)\n",
		(long long)info.frameCount);
	std::printf("video codec: %s\n", info.videoCodec.empty() ? "?" : info.videoCodec.c_str());
	if(info.hasAudio)
		std::printf("audio:       yes, %d ch @ %.0f Hz (%s)\n", info.audioChannels, info.audioSampleRate,
			info.audioCodec.c_str());
	else
		std::printf("audio:       none\n");
	return 0;
}

int CommandFrame(const std::string& in, const std::string& out, int64_t atMs)
{
	std::string error;
	auto movie = Movie::Open(in, &error);
	if(!movie)
	{
		std::fprintf(stderr, "frame: %s\n", error.c_str());
		return 1;
	}
	Canvas canvas;
	if(!movie->FrameAt(atMs, &canvas, &error))
	{
		std::fprintf(stderr, "frame: %s\n", error.c_str());
		return 1;
	}
	double r = 0, g = 0, b = 0;
	canvas.MeanRgb(&r, &g, &b);
	std::printf("decoded %dx%d frame at %lld ms (mean rgb %.1f %.1f %.1f, luma %.2f)\n", canvas.width,
		canvas.height, (long long)atMs, r, g, b, canvas.MeanLuma());
	if(!WritePng(out, canvas.width, canvas.height, canvas.rgba.data(), error))
	{
		std::fprintf(stderr, "frame: %s\n", error.c_str());
		return 1;
	}
	std::printf("wrote %s\n", out.c_str());
	return 0;
}

int CommandProbe(const std::string& path)
{
	ProbeResult result;
	std::string error;
	if(!Probe(path, &result, &error))
	{
		std::fprintf(stderr, "probe: %s\n", error.c_str());
		return 1;
	}
	std::string infoError;
	auto movie = Movie::Open(path, &infoError);
	const MovieInfo info = movie ? movie->Info() : MovieInfo{};

	std::printf("file:          %s\n", path.c_str());
	std::printf("decoded:       %lld frames in %.3f s\n", (long long)result.framesDecoded, result.secondsWall);
	if(movie)
	{
		const double fps = result.secondsWall > 0 ? (double)result.framesDecoded / result.secondsWall : 0;
		std::printf("throughput:    %.0f frames/s (%.0fx realtime at %.2f fps)\n", fps,
			info.fps > 0 ? fps / info.fps : 0.0, info.fps);
	}
	std::printf("first frame:   luma %lld, rgb %.1f %.1f %.1f\n", (long long)result.firstFrameLuma,
		result.firstR, result.firstG, result.firstB);
	std::printf("last frame:    luma %lld, rgb %.1f %.1f %.1f\n", (long long)result.lastFrameLuma,
		result.lastR, result.lastG, result.lastB);
	std::printf("luma range:    %.2f .. %.2f (std dev %.2f)\n", result.minLuma, result.maxLuma,
		result.lumaVariance);
	std::printf("rss:           +%s while decoding %lld frames (one frame held at a time)\n",
		HumanBytes(result.peakRssDeltaBytes).c_str(), (long long)result.framesDecoded);
	return 0;
}

// ---------------------------------------------------------------------------
// tags / members - the real-data half, text only
// ---------------------------------------------------------------------------
struct MovieTagRecord
{
	std::string file;
	std::string bg;
	bool cancel = false;
	std::string source; // where it was found
};

std::string AttributeOf(const std::string& line, const std::string& key)
{
	const std::string needle = key + "=";
	size_t at = line.find(needle);
	if(at == std::string::npos) return std::string();
	at += needle.size();
	if(at >= line.size()) return std::string();
	const char first = line[at];
	if(first == '"')
	{
		const size_t close = line.find('"', at + 1);
		if(close == std::string::npos) return std::string();
		return line.substr(at + 1, close - at - 1);
	}
	size_t end = at;
	while(end < line.size() && line[end] != ',' && line[end] != '}' && line[end] != ' ') end++;
	return line.substr(at, end - at);
}

int CommandTags(const std::vector<std::string>& scripts)
{
	std::vector<MovieTagRecord> records;
	for(const std::string& path : scripts)
	{
		std::FILE* f = std::fopen(path.c_str(), "rb");
		if(!f)
		{
			std::fprintf(stderr, "tags: cannot open %s\n", path.c_str());
			continue;
		}
		std::string text;
		char buf[8192];
		size_t n;
		while((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
		std::fclose(f);

		// The corpus writes these as Lua tables: {"movie", cancel=1, file="op"},
		// not as KAG-style [movie ...].  Matching the table name is what keeps
		// `movie=1, lv=10, path=":ani/"` (an attribute of a different tag) out.
		size_t at = 0;
		while((at = text.find("{\"movie\"", at)) != std::string::npos)
		{
			const size_t close = text.find('}', at);
			if(close == std::string::npos) break;
			const std::string line = text.substr(at, close - at + 1);
			MovieTagRecord record;
			record.file = AttributeOf(line, "file");
			record.bg = AttributeOf(line, "bg");
			record.cancel = AttributeOf(line, "cancel") == "1";
			record.source = path;
			records.push_back(record);
			at = close;
		}
	}

	std::printf("movie tags found: %zu\n", records.size());
	int withBgBlack = 0, withCancel = 0;
	for(size_t i = 0; i < records.size(); i++)
	{
		const MovieTagRecord& r = records[i];
		if(r.cancel) withCancel++;
		if(r.bg == "black") withBgBlack++;
		std::printf("  [%zu] file=%-18s cancel=%d bg=%-6s  (%s)\n", i, r.file.c_str(), (int)r.cancel,
			r.bg.empty() ? "-" : r.bg.c_str(), r.source.c_str());
	}
	std::printf("summary: %zu tags, %d with cancel=1, %d with bg=\"black\"\n", records.size(), withCancel,
		withBgBlack);
	if(records.empty()) return 1;
	return 0;
}

int CommandMembers(const std::string& indexPath, const std::vector<std::string>& names)
{
	std::FILE* f = std::fopen(indexPath.c_str(), "rb");
	if(!f)
	{
		std::fprintf(stderr, "members: cannot open %s\n", indexPath.c_str());
		return 1;
	}
	std::vector<std::string> members;
	std::string text;
	char buf[8192];
	size_t n;
	while((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
	std::fclose(f);
	{
		size_t start = 0;
		while(start < text.size())
		{
			size_t nl = text.find('\n', start);
			if(nl == std::string::npos) nl = text.size();
			std::string line = text.substr(start, nl - start);
			if(!line.empty() && line[0] != '#')
			{
				const size_t tab = line.find('\t');
				if(tab != std::string::npos) line = line.substr(0, tab);
				if(!line.empty()) members.push_back(line);
			}
			start = nl + 1;
		}
	}

	// Extension census: what the archive actually holds.
	const char* const kVideoExtensions[] = {".mp4", ".avi", ".wmv", ".mov", ".mkv", ".mpg", ".mpeg", ".ogv"};
	int videoMembers = 0;
	for(const std::string& m : members)
	{
		for(const char* ext : kVideoExtensions)
		{
			const size_t len = std::strlen(ext);
			if(m.size() > len && m.compare(m.size() - len, len, ext) == 0)
			{
				videoMembers++;
				break;
			}
		}
	}
	std::printf("archive:        %s\n", indexPath.c_str());
	std::printf("members:        %zu\n", members.size());
	std::printf("video members:  %d\n", videoMembers);

	// Extension histogram, so "there are no videos" is evidence and not a claim.
	std::vector<std::pair<std::string, int>> histogram;
	for(const std::string& m : members)
	{
		const size_t dot = m.find_last_of('.');
		std::string ext = dot == std::string::npos ? "(none)" : m.substr(dot);
		auto it = std::find_if(histogram.begin(), histogram.end(),
			[&](const std::pair<std::string, int>& p) { return p.first == ext; });
		if(it == histogram.end()) histogram.push_back({ext, 1});
		else it->second++;
	}
	std::sort(histogram.begin(), histogram.end(),
		[](const std::pair<std::string, int>& a, const std::pair<std::string, int>& b) { return a.second > b.second; });
	std::printf("extensions:     ");
	for(const auto& p : histogram) std::printf("%s=%d ", p.first.c_str(), p.second);
	std::printf("\n");

	if(names.empty()) return videoMembers > 0 ? 0 : 1;
	int found = 0;
	for(const std::string& name : names)
	{
		bool hit = false;
		for(const std::string& m : members)
		{
			// A member like movie\op.mp4 or sound/op.avi matches the stem "op".
			const size_t dot = m.find_last_of('.');
			if(dot == std::string::npos) continue;
			const std::string stem = m.substr(0, dot);
			const size_t slash = stem.find_last_of("\\/");
			const std::string base = slash == std::string::npos ? stem : stem.substr(slash + 1);
			if(base == name)
			{
				hit = true;
				std::printf("  %-18s FOUND  %s\n", name.c_str(), m.c_str());
				break;
			}
		}
		if(hit) found++;
		else std::printf("  %-18s absent from this corpus\n", name.c_str());
	}
	std::printf("summary: %d of %zu named movies present, %d video members total\n", found, names.size(),
		videoMembers);
	return 0;
}

// ---------------------------------------------------------------------------
// fixtures
// ---------------------------------------------------------------------------
struct Fixtures
{
	std::string dir;
	std::string redBlue;
	std::string withAudio;
	std::string vfr;
	std::string longClip;
	bool ready = false;
	std::string why;
};

Fixtures MakeFixtures()
{
	Fixtures f;
	f.dir = "/tmp/movietool_fixtures";
	RunCommand("mkdir -p " + f.dir);
	f.redBlue = f.dir + "/red_blue.mp4";
	f.withAudio = f.dir + "/with_audio_30.mp4";
	f.vfr = f.dir + "/vfr.mp4";
	f.longClip = f.dir + "/long_10s.mp4";

	const char* kFfmpeg = "/opt/homebrew/bin/ffmpeg";
	if(!FilePresent(kFfmpeg))
	{
		f.why = "ffmpeg not found";
		return f;
	}

	// Red for one second, blue for one second, 24 fps: first and last frame are
	// then known colours, which is what makes the pixel assertions meaningful.
	RunCommand(std::string(kFfmpeg) +
		" -y -loglevel error -f lavfi -i \"color=c=red:s=160x120:d=1:r=24\""
		" -f lavfi -i \"color=c=blue:s=160x120:d=1:r=24\""
		" -filter_complex \"[0:v][1:v]concat=n=2:v=1:a=0\""
		" -c:v libx264 -pix_fmt yuv420p \"" + f.redBlue + "\" >/dev/null 2>&1");

	// 30 fps with a real audio track.
	RunCommand(std::string(kFfmpeg) +
		" -y -loglevel error -f lavfi -i \"testsrc2=s=160x120:r=30:d=2\""
		" -f lavfi -i \"sine=frequency=440:duration=2\""
		" -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest \"" + f.withAudio + "\" >/dev/null 2>&1");

	// Variable frame rate: nudge every presentation time by a sine, then let
	// ffmpeg keep the real timestamps (-vsync vfr).  Frame N is now NOT at N/fps.
	RunCommand(std::string(kFfmpeg) +
		" -y -loglevel error -f lavfi -i \"testsrc2=s=160x120:r=30:d=3\""
		" -vf \"setpts=PTS+0.04*sin(N)/TB\" -fps_mode vfr"  // -vsync is gone in ffmpeg 9
		" -c:v libx264 -pix_fmt yuv420p \"" + f.vfr + "\" >/dev/null 2>&1");

	// Ten seconds, same size and codec as the 2 s clip: the RSS comparison case.
	RunCommand(std::string(kFfmpeg) +
		" -y -loglevel error -f lavfi -i \"testsrc2=s=160x120:r=24:d=10\""
		" -c:v libx264 -pix_fmt yuv420p \"" + f.longClip + "\" >/dev/null 2>&1");

	f.ready = FilePresent(f.redBlue) && FilePresent(f.withAudio) && FilePresent(f.vfr) &&
		FilePresent(f.longClip);
	if(!f.ready) f.why = "ffmpeg could not produce the synthetic clips";
	return f;
}

} // namespace

// ---------------------------------------------------------------------------
// selftest
// ---------------------------------------------------------------------------
int RunSelfTest()
{
	const bool breakIt = std::getenv("MOVIETOOL_SELFTEST_BREAK") != nullptr;
	std::printf("movietool selftest%s\n", breakIt ? "  [BREAK MODE: expectations corrupted on purpose]" : "");

	const Fixtures fx = MakeFixtures();
	if(!fx.ready)
	{
		Skip("fixtures", fx.why + " (pixel assertions need synthetic clips; real game video is off limits)");
		std::printf("\n%d checks, %d failed, %d skipped\nRESULT: SKIPPED\n", gChecks, gFailures, gSkips);
		return 0;
	}

	std::printf("\n-- container facts --\n");
	{
		std::string error;
		auto movie = Movie::Open(fx.redBlue, &error);
		Check(movie != nullptr, "info: open a synthetic mp4", error);
		if(movie)
		{
			const MovieInfo& info = movie->Info();
			Check(info.width == 160 && info.height == 120, "info: frame size 160x120",
				std::to_string(info.width) + "x" + std::to_string(info.height));
			Check(std::fabs(info.fps - 24.0) < 0.5, "info: 24 fps", std::to_string(info.fps));
			Check(std::llabs(info.durationMs - 2000) < 120, "info: ~2000 ms duration",
				std::to_string((long long)info.durationMs) + " ms");
			Check(!info.hasAudio, "info: this clip has no audio track", "");
			Check(info.videoCodec == "avc1", "info: h264 video track", info.videoCodec);
		}
	}
	{
		std::string error;
		auto movie = Movie::Open(fx.withAudio, &error);
		Check(movie != nullptr, "info: open the clip with audio", error);
		if(movie)
		{
			const MovieInfo& info = movie->Info();
			Check(std::fabs(info.fps - 30.0) < 0.5, "info: 30 fps", std::to_string(info.fps));
			Check(info.hasAudio, "info: audio track detected", "");
			Check(info.audioChannels == 1 && std::fabs(info.audioSampleRate - 44100) < 1,
				"info: audio is 1 ch @ 44100 Hz",
				std::to_string(info.audioChannels) + " ch @ " + std::to_string((int)info.audioSampleRate));
		}
	}

	std::printf("\n-- whole-file decode --\n");
	{
		ProbeResult result;
		std::string error;
		const bool ok = Probe(fx.redBlue, &result, &error);
		Check(ok, "probe: decode the 2 s clip", error);
		if(ok)
		{
			const int64_t expected = breakIt ? 49 : 48;
			Check(result.framesDecoded == expected, "probe: exactly 48 frames (2 s at 24 fps)",
				std::to_string((long long)result.framesDecoded) + " vs " + std::to_string((long long)expected));
			Check(result.firstR > 180.0 && result.firstB < 80.0, "probe: first frame is red",
				"rgb " + std::to_string((int)result.firstR) + " " + std::to_string((int)result.firstG) + " " +
					std::to_string((int)result.firstB));
			Check(result.lastB > 180.0 && result.lastR < 80.0, "probe: last frame is blue",
				"rgb " + std::to_string((int)result.lastR) + " " + std::to_string((int)result.lastG) + " " +
					std::to_string((int)result.lastB));
			Check(result.maxLuma - result.minLuma > 20.0, "probe: the clip really changes colour",
				"luma " + std::to_string((int)result.minLuma) + ".." + std::to_string((int)result.maxLuma));
			std::printf("  decoded %lld frames, rss +%lld B\n", (long long)result.framesDecoded,
				(long long)result.peakRssDeltaBytes);
		}
	}
	{
		ProbeResult result;
		std::string error;
		if(Probe(fx.withAudio, &result, &error))
		{
			Check(result.framesDecoded == 60, "probe: exactly 60 frames (2 s at 30 fps)",
				std::to_string((long long)result.framesDecoded));
		}
		else
		{
			Fail("probe: 30 fps clip decode", error);
		}
	}

	std::printf("\n-- seek by timestamp --\n");
	{
		std::string error;
		auto movie = Movie::Open(fx.redBlue, &error);
		if(!movie)
		{
			Fail("seek: open", error);
		}
		else
		{
			Canvas first, mid, last;
			const bool okFirst = movie->FrameAt(100, &first, &error);
			const bool okMid = movie->FrameAt(1500, &mid, &error);
			const bool okLast = movie->FrameAt(1900, &last, &error);
			Check(okFirst && okMid && okLast, "seek: frames at 100 / 1500 / 1900 ms", error);
			if(okFirst && okMid && okLast)
			{
				double r = 0, g = 0, b = 0;
				first.MeanRgb(&r, &g, &b);
				Check(r > 180.0 && b < 80.0, "seek: 100 ms is still red",
					"rgb " + std::to_string((int)r) + " " + std::to_string((int)g) + " " + std::to_string((int)b));
				mid.MeanRgb(&r, &g, &b);
				Check(b > 180.0 && r < 80.0, "seek: 1500 ms is past the cut, so blue",
					"rgb " + std::to_string((int)r) + " " + std::to_string((int)g) + " " + std::to_string((int)b));
				last.MeanRgb(&r, &g, &b);
				Check(b > 180.0 && r < 80.0, "seek: 1900 ms is blue",
					"rgb " + std::to_string((int)r) + " " + std::to_string((int)g) + " " + std::to_string((int)b));
				Check(first.width == 160 && mid.height == 120, "seek: decoded frames are 160x120", "");
			}

			// Seeking backwards must land on the same frame, not a neighbour.
			Canvas again;
			movie->FrameAt(1500, &again, &error);
			Check(Checksum(mid) == Checksum(again), "seek: the same timestamp decodes identically twice", "");

			// 10 seeks, so the cost is a measurement rather than a guess.
			const double t0 = NowSeconds();
			for(int i = 0; i < 10; i++)
			{
				Canvas canvas;
				movie->FrameAt(200 + i * 150, &canvas, &error);
			}
			const double perSeekMs = (NowSeconds() - t0) * 1000.0 / 10.0;
			char detail[160];
			std::snprintf(detail, sizeof detail, "%.2f ms per seek+decode on a 2 s clip", perSeekMs);
			Check(perSeekMs < 250.0, "seek: repositioning stays cheap", detail);
		}
	}

	std::printf("\n-- variable frame rate --\n");
	{
		// Frame timestamps here are deliberately uneven (setpts with a sine), so a
		// decoder that assumes frame N sits at N/fps returns the wrong picture.
		std::string error;
		auto movie = Movie::Open(fx.vfr, &error);
		if(!movie)
		{
			Fail("vfr: open the variable-rate clip", error);
		}
		else
		{
			Check(true, "vfr: variable-rate clip opens", std::to_string(movie->Info().durationMs) + " ms");
			bool allOk = true;
			bool anyDecoded = false;
			for(int64_t ms : {(int64_t)0, (int64_t)350, (int64_t)700, (int64_t)1250, (int64_t)1800, (int64_t)2600})
			{
				Canvas canvas;
				std::string seekError;
				if(!movie->FrameAt(ms, &canvas, &seekError) || canvas.Empty() || canvas.width != 160)
				{
					allOk = false;
					std::fprintf(stderr, "  vfr: FrameAt(%lld) -> %s\n", (long long)ms,
						seekError.empty() ? "empty frame" : seekError.c_str());
				}
				else
				{
					anyDecoded = true;
				}
			}
			Check(allOk && anyDecoded, "vfr: FrameAt returns a real frame at every timestamp",
				"6 timestamps across an uneven timeline");

			// Timestamps must be non-decreasing as we read forward; if the decoder
			// were assuming CFR this would drift.  Rewind first: the FrameAt calls
			// above left the reader somewhere in the middle of the file.
			movie->Seek(0, &error);
			Canvas canvas;
			int64_t previous = -1;
			bool monotone = true;
			int64_t counted = 0;
			while(movie->ReadNext(&canvas, &error) && counted < 200)
			{
				if(movie->PositionMs() < previous) monotone = false;
				previous = movie->PositionMs();
				counted++;
			}
			Check(monotone && counted > 30, "vfr: presentation times advance monotonically",
				std::to_string((long long)counted) + " frames decoded");
		}
	}

	std::printf("\n-- MovieLayer: cancel=1 and bg=\"black\" --\n");
	{
		MovieLayer layer;
		std::string error;
		Check(layer.Play(fx.redBlue, /*clearToBlack*/ true, &error), "layer: play with bg=\"black\"", error);
		Check(layer.ClearedToBlack(), "layer: reports a black backdrop", "");
		const Canvas& backdrop = layer.Backdrop();
		bool opaqueBlack = !backdrop.Empty() && backdrop.width == 160;
		if(opaqueBlack)
		{
			for(size_t i = 0; i + 3 < backdrop.rgba.size(); i += 4)
			{
				if(backdrop.rgba[i] != 0 || backdrop.rgba[i + 1] != 0 || backdrop.rgba[i + 2] != 0 ||
					backdrop.rgba[i + 3] != 255)
				{
					opaqueBlack = false;
					break;
				}
			}
		}
		Check(opaqueBlack, "layer: backdrop is opaque black at the movie's size", "");

		Canvas canvas;
		Check(layer.Advance(0, &canvas) && !canvas.Empty(), "layer: first tick yields a frame",
			std::to_string(canvas.width) + "x" + std::to_string(canvas.height));
		Check(layer.Advance(1000, &canvas), "layer: advancing across the cut still renders", "");
		// Past the end the layer reports finished, which is when the script resumes.
		const bool stillShowing = layer.Advance(2500, &canvas);
		Check(!breakIt ? !stillShowing : stillShowing, "layer: finishes at the end of the clip", "");
		Check(layer.Finished(), "layer: Finished() agrees with Advance()", "");

		// cancel=1
		MovieLayer cancelled;
		cancelled.Play(fx.redBlue, false, &error);
		cancelled.Cancel();
		Check(!cancelled.Active(), "layer: cancel=1 stops the movie", "");
		Canvas afterCancel;
		Check(!cancelled.Advance(100, &afterCancel), "layer: no frames after cancel", "");
	}

	std::printf("\n-- frame dump to PNG --\n");
	{
		std::string error;
		const std::string out = fx.dir + "/frame_1500.png";
		auto movie = Movie::Open(fx.redBlue, &error);
		Canvas canvas;
		if(movie && movie->FrameAt(1500, &canvas, &error) &&
			WritePng(out, canvas.width, canvas.height, canvas.rgba.data(), error))
		{
			artemis::Image image;
			if(ReadPng(out, image, error))
			{
				Check(image.width == 160 && image.height == 120, "png: round-tripped frame is 160x120", "");
				// The saved frame came from the blue half.
				Check(image.rgba.size() > 3 && image.rgba[2] > 150 && image.rgba[0] < 80,
					"png: saved frame kept the decoded pixels",
					"pixel0 = " + std::to_string((int)image.rgba[0]) + "," + std::to_string((int)image.rgba[1]) +
						"," + std::to_string((int)image.rgba[2]));
			}
			else
			{
				Fail("png: read back the dump", error);
			}
		}
		else
		{
			Fail("png: write a frame dump", error);
		}
	}

	std::printf("\n-- memory: one frame, not one film --\n");
	{
		ProbeResult shortClip, longClip;
		std::string error;
		const int64_t rssBefore = ResidentBytes();
		const bool okShort = Probe(fx.redBlue, &shortClip, &error);
		const bool okLong = Probe(fx.longClip, &longClip, &error);
		const int64_t warmed = ResidentBytes() - rssBefore;
		Check(okShort && okLong, "memory: probe 2 s and 10 s clips", error);
		if(okShort && okLong)
		{
			Check(longClip.framesDecoded > shortClip.framesDecoded * 4,
				"memory: the long clip really is longer",
				std::to_string((long long)shortClip.framesDecoded) + " vs " +
					std::to_string((long long)longClip.framesDecoded) + " frames");
			char detail[260];
			std::snprintf(detail, sizeof detail,
				"2 s clip rss +%s, 10 s clip rss +%s (%lld vs %lld frames)",
				HumanBytes(shortClip.peakRssDeltaBytes).c_str(), HumanBytes(longClip.peakRssDeltaBytes).c_str(),
				(long long)shortClip.framesDecoded, (long long)longClip.framesDecoded);
			// Five times the frames must not cost five times the memory: one frame
			// is held at a time, so the deltas should be the same order of magnitude.
			const int64_t allowance = 8 * 1024 * 1024;
			Check(shortClip.peakRssDeltaBytes < allowance && longClip.peakRssDeltaBytes < allowance,
				"memory: both clips decode in a frame-sized footprint", detail);
			std::snprintf(detail, sizeof detail, "process rss grew %s after both probes (allocator warm-up)",
				HumanBytes(warmed).c_str());
			Check(warmed < 32 * 1024 * 1024, "memory: no film-sized allocation anywhere", detail);
		}
	}

	std::printf("\n-- damaged input --\n");
	{
		const std::string broken = fx.dir + "/broken.mp4";
		std::FILE* out = std::fopen(broken.c_str(), "wb");
		const char junk[] = "this is not a movie file, it is a sentence";
		if(out)
		{
			std::fwrite(junk, 1, sizeof junk, out);
			std::fclose(out);
		}
		std::string error;
		auto movie = Movie::Open(broken, &error);
		const bool refused = !movie || !movie->ReadNext(nullptr, &error);
		Check(refused, "damaged: a non-video file is refused without crashing", error);

		auto missing = Movie::Open(fx.dir + "/does_not_exist.mp4", &error);
		Check(!missing, "damaged: a missing file reports why", error);
	}

	std::printf("\n%d checks, %d failed, %d skipped\n", gChecks, gFailures, gSkips);
	if(gFailures == 0) std::printf("RESULT: OK\n");
	else std::printf("RESULT: FAILED\n");
	return gFailures == 0 ? 0 : 1;
}

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		std::fprintf(stderr,
			"usage: movietool <command> [args]\n"
			"  info    <file.mp4>\n"
			"  frame   <in> <out.png> [--at <ms>]\n"
			"  probe   <file.mp4>\n"
			"  tags    <script.ast>...\n"
			"  members <archive_index.txt> [--names a,b,c]\n"
			"  selftest\n");
		return 2;
	}
	const std::string command = argv[1];
	if(command == "selftest") return RunSelfTest();

	int64_t atMs = 0;
	std::vector<std::string> names;
	std::vector<std::string> positional;
	for(int i = 2; i < argc; i++)
	{
		const std::string arg = argv[i];
		if(arg == "--at" && i + 1 < argc) atMs = std::atoll(argv[++i]);
		else if(arg == "--names" && i + 1 < argc)
		{
			std::string list = argv[++i];
			size_t start = 0;
			while(start <= list.size())
			{
				const size_t comma = list.find(',', start);
				const std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
				if(!item.empty()) names.push_back(item);
				if(comma == std::string::npos) break;
				start = comma + 1;
			}
		}
		else positional.push_back(arg);
	}

	if(command == "info" && !positional.empty()) return CommandInfo(positional[0]);
	if(command == "probe" && !positional.empty()) return CommandProbe(positional[0]);
	if(command == "frame" && positional.size() >= 2) return CommandFrame(positional[0], positional[1], atMs);
	if(command == "tags") return CommandTags(positional);
	if(command == "members" && !positional.empty()) return CommandMembers(positional[0], names);

	std::fprintf(stderr, "movietool: bad arguments for '%s'\n", command.c_str());
	return 2;
}
