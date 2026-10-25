// audiotool.cpp - inspect, probe and self-test the Artemis audio layer
//
// Every number this tool prints is meant to be checkable without listening:
//
//   audiotool info  <file.ogg>              container facts (cross-check: afinfo)
//   audiotool probe <file.ogg> --seconds N  decode to PCM, report RMS/peak/...
//   audiotool sli   <file.sli> [--ogg ..]   loop points and the unit verdict
//   audiotool sli-batch <dir>               aggregate evidence over many .sli
//   audiotool play  <file.ogg> [--loop] [--seconds N]
//                                           render through the real Engine path
//   audiotool selftest                      assertions on real game data
//
// `play` renders into a PCM buffer instead of opening a device: the session may
// have no audio hardware, and the interesting question is whether the mixer
// produces the right samples, not whether speakers are attached.  Engine::Mix
// is the same entry point an SDL/CoreAudio callback would call, so the path
// exercised here is the real one; only the final hand-off is stubbed.

#include "../src/audio/audio.h"
#include "../src/audio/device.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace artemis::audio;

namespace {

int gFailures = 0;
int gChecks = 0;
int gSkips = 0;

void Pass(const char* what, const std::string& detail = std::string()) {
	gChecks++;
	std::printf("  \033[32mPASS\033[0m %-52s %s\n", what, detail.c_str());
}

void Fail(const char* what, const std::string& detail) {
	gChecks++;
	gFailures++;
	std::printf("  \033[31mFAIL\033[0m %-52s %s\n", what, detail.c_str());
}

void Skip(const char* what, const std::string& why) {
	gSkips++;
	std::printf("  \033[33mSKIP\033[0m %-52s %s\n", what, why.c_str());
}

void Check(bool ok, const char* what, const std::string& detail) {
	if (ok)
		Pass(what, detail);
	else
		Fail(what, detail);
}

std::string HumanBytes(int64_t bytes) {
	char buf[64];
	if (bytes >= 1024 * 1024)
		std::snprintf(buf, sizeof buf, "%.2f MB", (double)bytes / (1024.0 * 1024.0));
	else if (bytes >= 1024)
		std::snprintf(buf, sizeof buf, "%.1f KB", (double)bytes / 1024.0);
	else
		std::snprintf(buf, sizeof buf, "%lld B", (long long)bytes);
	return buf;
}

std::string FormatSeconds(double s) {
	char buf[64];
	const int minutes = (int)(s / 60.0);
	const double rest = s - minutes * 60.0;
	if (minutes > 0)
		std::snprintf(buf, sizeof buf, "%d:%05.2f", minutes, rest);
	else
		std::snprintf(buf, sizeof buf, "%.3f s", rest);
	return buf;
}

// ---------------------------------------------------------------------------
// statistics over decoded PCM
// ---------------------------------------------------------------------------
struct PcmStats {
	int64_t frames = 0;
	double sumSquares = 0.0;
	double peak = 0.0;
	int64_t silentFrames = 0;
	double channelDiffSquares = 0.0;
	int channels = 0;

	void Add(const float* p, int64_t frames, int ch) {
		channels = ch;
		for (int64_t i = 0; i < frames; i++) {
			const float* f = p + i * ch;
			double framePeak = 0.0;
			for (int c = 0; c < ch; c++) {
				const double v = f[c];
				sumSquares += v * v;
				if (std::fabs(v) > framePeak) framePeak = std::fabs(v);
			}
			if (framePeak > peak) peak = framePeak;
			if (framePeak <= (double)kSilenceFloor) silentFrames++;
			if (ch >= 2) {
				const double d = (double)f[0] - (double)f[1];
				channelDiffSquares += d * d;
			}
		}
		this->frames += frames;
	}
	double Rms() const { return frames ? std::sqrt(sumSquares / ((double)frames * (double)channels)) : 0.0; }
	double ChannelDiffRms() const { return frames ? std::sqrt(channelDiffSquares / (double)frames) : 0.0; }
	double SilentFraction() const { return frames ? (double)silentFrames / (double)frames : 1.0; }
	double PeakDbfs() const { return peak > 0 ? 20.0 * std::log10(peak) : -200.0; }
};

// ---------------------------------------------------------------------------
// info
// ---------------------------------------------------------------------------
int CommandInfo(const std::string& path) {
	std::string error;
	auto source = Source::Open(path, &error);
	if (!source) {
		std::fprintf(stderr, "info: %s\n", error.c_str());
		return 1;
	}
	const TrackInfo& info = source->Info();
	std::printf("file:        %s\n", path.c_str());
	std::printf("codec:       %s (Ogg Vorbis via CoreAudio ExtAudioFile)\n", info.codec.c_str());
	std::printf("sample rate: %d Hz\n", info.sampleRate);
	std::printf("channels:    %d (%s)\n", info.channels,
	            info.channels == 1 ? "mono" : info.channels == 2 ? "stereo" : "multichannel");
	std::printf("frames:      %lld\n", (long long)info.frames);
	std::printf("duration:    %s\n", FormatSeconds(info.Seconds()).c_str());
	std::printf("size:        %s (%lld bytes)\n", HumanBytes(info.fileBytes).c_str(), (long long)info.fileBytes);
	if (info.Seconds() > 0)
		std::printf("bit rate:    %.0f kbps\n", (double)info.fileBytes * 8.0 / info.Seconds() / 1000.0);
	return 0;
}

// ---------------------------------------------------------------------------
// probe
// ---------------------------------------------------------------------------
int CommandProbe(const std::string& path, double seconds) {
	std::string error;
	const int64_t rssBefore = ResidentBytes();
	auto source = Source::Open(path, &error);
	if (!source) {
		std::fprintf(stderr, "probe: %s\n", error.c_str());
		return 1;
	}
	const TrackInfo& info = source->Info();
	const int64_t rssAfterOpen = ResidentBytes();

	const int64_t wantFrames = seconds > 0 ? (int64_t)(seconds * info.sampleRate) : info.frames;
	const int blockFrames = 4096;
	std::vector<float> buffer((size_t)blockFrames * (size_t)info.channels);

	PcmStats stats;
	int64_t remaining = std::min(wantFrames, info.frames);
	const double t0 = NowSeconds();
	while (remaining > 0) {
		const int64_t ask = std::min<int64_t>(blockFrames, remaining);
		const int64_t got = source->Read(buffer.data(), ask);
		if (got <= 0) break;
		stats.Add(buffer.data(), got, info.channels);
		remaining -= got;
	}
	const double elapsed = NowSeconds() - t0;
	const int64_t rssAfterDecode = ResidentBytes();

	const double audioSeconds = (double)stats.frames / info.sampleRate;
	std::printf("file:            %s\n", path.c_str());
	std::printf("decoded:         %lld frames (%s) of %lld available\n", (long long)stats.frames,
	            FormatSeconds(audioSeconds).c_str(), (long long)info.frames);
	std::printf("decoded bytes:   %s as float32\n",
	            HumanBytes(stats.frames * info.channels * 4).c_str());
	std::printf("rms:             %.6f  (%.2f dBFS)\n", stats.Rms(),
	            stats.Rms() > 0 ? 20.0 * std::log10(stats.Rms()) : -200.0);
	std::printf("peak:            %.6f  (%.2f dBFS)\n", stats.peak, stats.PeakDbfs());
	std::printf("silent frames:   %.2f%%  (|sample| <= %.2g)\n", stats.SilentFraction() * 100.0,
	            (double)kSilenceFloor);
	std::printf("silent overall:  %s\n", stats.SilentFraction() >= 0.999 ? "yes" : "no");
	if (info.channels >= 2) {
		std::printf("channel diff L-R: rms %.6f (%s)\n", stats.ChannelDiffRms(),
		            stats.ChannelDiffRms() > 1e-6 ? "channels differ (not a dual-mono fake)"
		                                          : "channels identical (dual mono)");
	} else {
		std::printf("channel diff L-R: n/a (mono)\n");
	}
	std::printf("decode time:     %.3f s wall\n", elapsed);
	if (elapsed > 0) {
		std::printf("throughput:      %.1fx realtime, %.1f MB/s (of %lld-byte file)\n",
		            audioSeconds / elapsed,
		            (double)(info.fileBytes * (int64_t)((double)stats.frames / (double)std::max<int64_t>(1, info.frames))) /
		                elapsed / (1024.0 * 1024.0),
		            (long long)info.fileBytes);
	}
	std::printf("rss:             %s -> %s -> %s  (open +%s, decode +%s)\n",
	            HumanBytes(rssBefore).c_str(), HumanBytes(rssAfterOpen).c_str(),
	            HumanBytes(rssAfterDecode).c_str(), HumanBytes(rssAfterOpen - rssBefore).c_str(),
	            HumanBytes(rssAfterDecode - rssAfterOpen).c_str());
	return 0;
}

// ---------------------------------------------------------------------------
// sli
// ---------------------------------------------------------------------------
const char* UnitName(SliUnit unit) {
	switch (unit) {
	case SliUnit::Samples: return "samples";
	case SliUnit::Milliseconds: return "milliseconds";
	default: return "unknown";
	}
}

int CommandSli(const std::string& path, const std::string& oggPath, int64_t givenFrames, int givenRate) {
	SliInfo sli;
	std::string error;
	if (!sli.Load(path, &error)) {
		std::fprintf(stderr, "sli: %s\n", error.c_str());
		return 1;
	}
	std::printf("file:    %s\n", path.c_str());
	std::printf("version: %s\n", sli.version.c_str());
	std::printf("labels:  %zu\n", sli.labels.size());
	for (size_t i = 0; i < sli.labels.size(); i++)
		std::printf("  [%zu] Position=%-8lld Name='%s'\n", i, (long long)sli.labels[i].position,
		            sli.labels[i].name.c_str());

	// Decide the unit against the ogg the .sli describes.
	std::string resolved = oggPath;
	if (resolved.empty()) {
		if (path.size() > 4 && path.compare(path.size() - 4, 4, ".sli") == 0) {
			resolved = path.substr(0, path.size() - 4); // foo.ogg.sli -> foo.ogg
		} else {
			resolved = path + ".ogg";
		}
	}

	// The .ogg may live inside an archive where this tool cannot reach it.  In
	// that case the caller can supply the container facts (sample rate and frame
	// count come out of the Ogg headers, cross-checkable with afinfo) and the
	// unit verdict is computed just the same.
	TrackInfo info;
	std::string sourceLabel = resolved;
	if (givenFrames > 0 && givenRate > 0) {
		info.sampleRate = givenRate;
		info.frames = givenFrames;
		sourceLabel = "<supplied container facts>";
	} else {
		auto source = Source::Open(resolved, &error);
		if (!source) {
			std::printf("unit:    undetermined (no paired ogg: %s)\n", error.c_str());
			std::printf("         pass --frames N --rate R if the ogg lives inside an archive\n");
			return 0;
		}
		info = source->Info();
	}
	sli.unit = ResolveSliUnit(sli, info.frames, info.sampleRate);
	std::printf("paired ogg: %s  %d Hz, %lld frames, %s\n", sourceLabel.c_str(), info.sampleRate,
	            (long long)info.frames, FormatSeconds(info.Seconds()).c_str());

	const int64_t maxPos = sli.MaxPosition();
	const int64_t durationMs = info.frames * 1000 / info.sampleRate;
	std::printf("unit verdict: %s\n", UnitName(sli.unit));
	std::printf("  evidence: Position max %lld\n", (long long)maxPos);
	std::printf("    as samples -> %lld of %lld frames (%.1f%% into the clip) %s\n", (long long)maxPos,
	            (long long)info.frames, 100.0 * (double)maxPos / (double)info.frames,
	            maxPos <= info.frames ? "fits" : "OUT OF RANGE");
	std::printf("    as ms      -> %lld of %lld ms (%.1f%% of the clip length) %s\n", (long long)maxPos,
	            (long long)durationMs, 100.0 * (double)maxPos / (double)std::max<int64_t>(1, durationMs),
	            maxPos <= durationMs ? "fits" : "OUT OF RANGE");

	LoopPlan plan = LoopPlan::FromSli(sli, info.frames, info.sampleRate);
	std::printf("A-B loop: intro [0, %lld) then repeat [%lld, %lld) forever\n",
	            (long long)plan.introEnd, (long long)plan.loopStart, (long long)plan.loopEnd);
	std::printf("  intro  %s, loop body %s\n",
	            FormatSeconds((double)plan.introEnd / info.sampleRate).c_str(),
	            FormatSeconds((double)(plan.loopEnd - plan.loopStart) / info.sampleRate).c_str());
	return 0;
}

int CommandSliBatch(const std::string& dir) {
	// Aggregate the unit evidence over a directory of real .sli files.  Run
	// against extracted archive members this reproduces the numbers quoted in
	// audio.h: 388 labels, every position a multiple of 128, positions that
	// cannot be milliseconds.
	std::string command = "ls " + dir + "/*.sli 2>/dev/null";
	std::FILE* pipe = popen(command.c_str(), "r");
	if (!pipe) {
		std::fprintf(stderr, "sli-batch: cannot list %s\n", dir.c_str());
		return 1;
	}
	std::vector<std::string> files;
	char line[4096];
	while (std::fgets(line, sizeof line, pipe)) {
		std::string s(line);
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
		if (!s.empty()) files.push_back(s);
	}
	pclose(pipe);
	if (files.empty()) {
		std::fprintf(stderr, "sli-batch: no .sli files in %s\n", dir.c_str());
		return 1;
	}

	int64_t totalLabels = 0;
	int aligned128 = 0, aligned256 = 0, aligned512 = 0;
	int64_t minPos = INT64_MAX, maxPos = 0;
	int withVersion = 0;
	for (const std::string& f : files) {
		SliInfo sli;
		if (!sli.Load(f, nullptr)) continue;
		if (!sli.version.empty()) withVersion++;
		for (const SliLabel& l : sli.labels) {
			totalLabels++;
			if (l.position % 128 == 0) aligned128++;
			if (l.position % 256 == 0) aligned256++;
			if (l.position % 512 == 0) aligned512++;
			minPos = std::min(minPos, l.position);
			maxPos = std::max(maxPos, l.position);
		}
	}
	std::printf("files:           %zu\n", files.size());
	std::printf("with version:    %d\n", withVersion);
	std::printf("labels:          %lld\n", (long long)totalLabels);
	std::printf("position range:  %lld .. %lld\n", (long long)minPos, (long long)maxPos);
	std::printf("multiples of 128: %d / %lld\n", aligned128, (long long)totalLabels);
	std::printf("multiples of 256: %d / %lld\n", aligned256, (long long)totalLabels);
	std::printf("multiples of 512: %d / %lld\n", aligned512, (long long)totalLabels);
	std::printf("if milliseconds: longest loop point = %.1f s\n", (double)maxPos / 1000.0);
	std::printf("if samples@44100: longest loop point = %.3f s\n", (double)maxPos / 44100.0);
	std::printf("\nA position that is a multiple of 128 in %d/%lld cases is a sample-domain\n"
	            "quantity: milliseconds would hit that alignment by chance 1 time in 128.\n",
	            aligned128, (long long)totalLabels);
	return 0;
}

// ---------------------------------------------------------------------------
// render helpers
// ---------------------------------------------------------------------------
bool RenderStream(Stream& stream, double seconds, int rate, std::vector<float>* out) {
	const int64_t total = (int64_t)(seconds * rate);
	out->assign((size_t)total * 2, 0.0f);
	const int tick = 512;
	std::vector<float> block((size_t)tick * 2, 0.0f);
	int64_t done = 0;
	while (done < total) {
		const int n = (int)std::min<int64_t>(tick, total - done);
		// Stand in for the decoder thread.
		for (int tries = 0; tries < 256 && stream.Buffered() < n; tries++) {
			if (stream.Fill() == 0 && (stream.Finished() || stream.Stopped())) break;
		}
		std::fill(block.begin(), block.end(), 0.0f);
		stream.Mix(block.data(), n);
		std::memcpy(&(*out)[(size_t)done * 2], block.data(), (size_t)n * 2 * sizeof(float));
		done += n;
	}
	return true;
}

// A real device calls Mix() once per period; decoding runs ahead in its own
// thread.  A headless loop would otherwise drain the mixer faster than the
// decoder can fill it and report silence, so wait for the buffer - the audio
// equivalent of the device clock, without actually sleeping for real audio
// seconds.
void WaitForMix(Engine& engine, int frames) {
	for (int i = 0; i < 1000 && engine.BufferedFrames() < frames; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
}

double WindowDiff(const std::vector<float>& a, int64_t aStartFrame, const std::vector<float>& b,
                  int64_t bStartFrame, int64_t frames) {
	double worst = 0.0;
	for (int64_t i = 0; i < frames; i++)
		for (int c = 0; c < 2; c++) {
			const double d = std::fabs((double)a[(size_t)((aStartFrame + i) * 2 + c)] -
			                           (double)b[(size_t)((bStartFrame + i) * 2 + c)]);
			if (d > worst) worst = d;
		}
	return worst;
}

// ---------------------------------------------------------------------------
// play - headless, through the real Engine
// ---------------------------------------------------------------------------
int CommandPlay(const std::string& path, bool loop, double seconds, const std::string& seName) {
	// Engine resolves script names against a root, so hand it the file's own
	// directory and let it find the basename.
	std::string dir = ".";
	std::string base = path;
	const size_t slash = path.find_last_of('/');
	if (slash != std::string::npos) {
		dir = path.substr(0, slash);
		base = path.substr(slash + 1);
	}

	Engine engine;
	Engine::Config cfg;
	cfg.assetRoot = dir;
	cfg.sampleRate = 44100;
	std::string error;
	if (!engine.Open(cfg, &error)) {
		std::fprintf(stderr, "play: %s\n", error.c_str());
		return 1;
	}
	if (!engine.PlayBgm(base, loop, 0)) {
		std::fprintf(stderr, "play: cannot play %s\n", base.c_str());
		return 1;
	}
	if (!seName.empty() && !engine.PlaySe(seName, -1))
		std::fprintf(stderr, "play: cannot play se %s\n", seName.c_str());

	const int rate = cfg.sampleRate;
	const int tick = 1024;
	const int64_t total = (int64_t)(seconds * rate);
	std::vector<float> block((size_t)tick * 2);
	PcmStats stats;
	int64_t done = 0;
	const double t0 = NowSeconds();
	while (done < total) {
		const int n = (int)std::min<int64_t>(tick, total - done);
		WaitForMix(engine, n);
		engine.Mix(block.data(), n);
		stats.Add(block.data(), n, 2);
		done += n;
	}
	// Let the mixer settle so the reported stats cover real audio, then take a
	// second of measurement.
	PcmStats measured;
	for (int i = 0; i < rate / tick; i++) {
		engine.Mix(block.data(), tick);
		measured.Add(block.data(), tick, 2);
	}
	const double elapsed = NowSeconds() - t0;

	std::printf("file:        %s\n", path.c_str());
	std::printf("loop:        %s\n", loop ? "yes (A-B repeat)" : "no");
	std::printf("rendered:    %lld frames (%s) in %.3f s wall\n", (long long)stats.frames,
	            FormatSeconds((double)stats.frames / rate).c_str(), elapsed);
	std::printf("rms:         %.6f, peak %.6f (%.2f dBFS)\n", measured.Rms(), measured.peak,
	            measured.PeakDbfs());
	std::printf("silent:      %s\n", measured.SilentFraction() >= 0.999 ? "yes" : "no");
	std::printf("active:      %d stream(s), bgm '%s'\n", engine.ActiveStreams(),
	            engine.CurrentBgm().c_str());
	std::printf("device:      none (rendered to PCM; Engine::Mix is the callback entry)\n");
	engine.Shutdown();
	return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// selftest
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// devices / device - the real hardware path
// ---------------------------------------------------------------------------
int CommandDevices() {
	const DeviceList list = Device::Enumerate();
	std::printf("devices with output channels: %zu\n", list.devices.size());
	for (size_t i = 0; i < list.devices.size(); i++)
		std::printf("  [%zu]%s %-28s %d ch  %.0f Hz\n", i, (int)i == list.defaultIndex ? " *" : "  ",
		            list.devices[i].name.c_str(), list.devices[i].outputChannels, list.devices[i].sampleRate);
	const DeviceTarget target = Device::QueryDefault();
	if (!target.available) {
		std::printf("default output: unavailable - %s\n", target.reason.c_str());
		return 1;
	}
	std::printf("default output: %s  %.0f Hz  %d ch  buffer %d frames\n", target.name.c_str(), target.sampleRate,
	            target.channels, target.bufferFrames);
	return 0;
}

int CommandDevice(const std::string& path, bool loop, double seconds, const std::string& seName) {
	std::string dir = ".";
	std::string base = path;
	const size_t slash = path.find_last_of('/');
	if (slash != std::string::npos) {
		dir = path.substr(0, slash);
		base = path.substr(slash + 1);
	}

	const DeviceTarget target = Device::QueryDefault();
	if (!target.available) {
		std::printf("device: no output device - %s\n", target.reason.c_str());
		std::printf("device: FALLING BACK to offline rendering (no silent failure)\n");
		return CommandPlay(path, loop, seconds, seName);
	}

	std::printf("device: %s  %.0f Hz  %d ch  buffer %d frames\n", target.name.c_str(), target.sampleRate,
	            target.channels, target.bufferFrames);

	Engine engine;
	Engine::Config cfg;
	cfg.assetRoot = dir;
	cfg.sampleRate = (int)target.sampleRate; // mixer runs at the device rate
	std::string error;
	if (!engine.Open(cfg, &error)) {
		std::fprintf(stderr, "device: engine: %s\n", error.c_str());
		return 1;
	}
	if (!engine.PlayBgm(base, loop, 0)) {
		std::fprintf(stderr, "device: cannot play %s\n", base.c_str());
		return 1;
	}
	if (!seName.empty()) engine.PlaySe(seName, -1);

	Device device;
	std::string reason;
	if (!device.Open(&engine, target, &reason) || !device.Start(&reason)) {
		std::printf("device: FALLING BACK to offline rendering: %s\n", reason.c_str());
		engine.Shutdown();
		return CommandPlay(path, loop, seconds, seName);
	}

	std::printf("device: playing %s for %.1f s on real hardware (listen)\n", base.c_str(), seconds);
	std::this_thread::sleep_for(std::chrono::milliseconds((int)(seconds * 1000.0)));
	device.RefreshStats();
	const CallbackStats st = device.Stats();
	const double periodMs = 1000.0 * (double)device.BufferFrames() / target.sampleRate;
	std::printf("callbacks:   %llu over %llu frames\n", (unsigned long long)st.callbacks,
	            (unsigned long long)st.frames);
	std::printf("interval:    p50 %.3f ms  p99 %.3f ms  max %.3f ms  (period %.3f ms)\n", st.intervalP50Ms,
	            st.intervalP99Ms, st.intervalMaxMs, periodMs);
	std::printf("jitter:      %.4f ms (std dev around p50)\n", st.intervalJitterMs);
	std::printf("work:        p50 %.1f us  p99 %.1f us  max %.1f us\n", st.workP50Us, st.workP99Us, st.workMaxUs);
	std::printf("underruns:   %llu\n", (unsigned long long)st.underruns);
	std::printf("dropped:     %llu timing samples (capture array full)\n", (unsigned long long)st.dropped);
	device.Stop();
	device.Close();
	engine.Shutdown();
	return 0;
}

// ===========================================================================
// selftest - assertions over real game data
// ===========================================================================
namespace {

// Real .sli members pulled out of a shipped archive with
//   pfstool cat <arch.pfs> 'sound\vo\<name>.ogg.sli'
// They are text metadata only: loop points plus katakana label names.  They are
// embedded so the byte-exact round-trip assertion still runs on a machine
// without the archive.
const char* const kRealSliOneLabel = R"SLI(#2.00
# Sound Loop Information (utf-8)
# Generated by WaveLoopManager.cpp
Label { Position=139776;           Name='ラベル0';                                }
)SLI";
const char* const kRealSliFiveLabels = R"SLI(#2.00
# Sound Loop Information (utf-8)
# Generated by WaveLoopManager.cpp
Label { Position=63488;            Name='ラベル0';                                }
Label { Position=206848;           Name='ラベル1';                                }
Label { Position=273408;           Name='ラベル2';                                }
Label { Position=405504;           Name='ラベル3';                                }
Label { Position=455680;           Name='ラベル4';                                }
)SLI";
// This one belongs to a 12.667 s voice clip and is the sample that settles the
// unit question: its largest Position is 509952.
const char* const kRealSliPaired = R"SLI(#2.00
# Sound Loop Information (utf-8)
# Generated by WaveLoopManager.cpp
Label { Position=77824;            Name='ラベル0';                                }
Label { Position=208896;           Name='ラベル1';                                }
Label { Position=292864;           Name='ラベル2';                                }
Label { Position=394240;           Name='ラベル3';                                }
Label { Position=509952;           Name='ラベル4';                                }
)SLI";

// Container facts for kRealSliPaired's .ogg, read out of its Ogg granule
// headers (no audio decoded; the number is what `afinfo` reports as well):
// 558600 frames at 44100 Hz.
constexpr int64_t kPairedFrames = 558600;
constexpr int kPairedRate = 44100;

std::string Locate(const std::string& hint, const std::string& relative) {
	std::vector<std::string> dirs;
	if (!hint.empty()) dirs.push_back(hint);
	dirs.push_back("testdata");
	dirs.push_back("../testdata");
	dirs.push_back("../../testdata");
	for (const std::string& d : dirs) {
		std::string p = d + "/" + relative;
		std::FILE* f = std::fopen(p.c_str(), "rb");
		if (f) {
			std::fclose(f);
			return p;
		}
	}
	return std::string();
}

uint64_t Checksum(const float* p, size_t n) {
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < n; i++) {
		uint64_t bits = 0;
		std::memcpy(&bits, &p[i], sizeof(float));
		h = (h ^ bits) * 1099511628211ull;
	}
	return h;
}

} // namespace

int RunSecondBatch(const std::string& dataHint);

int RunSelfTest(const std::string& dataHint) {
	const bool breakIt = std::getenv("AUDIOTOOL_SELFTEST_BREAK") != nullptr;
	std::printf("audiotool selftest%s\n", breakIt ? "  [BREAK MODE: expectations corrupted on purpose]" : "");
	std::printf("\n-- .sli text layer --\n");

	// (1) The parser must accept the shipped byte layout ...
	{
		SliInfo sli;
		std::string error;
		const bool parsed = sli.Parse(kRealSliOneLabel, &error);
		Check(parsed, "sli: parse shipped single-label file", parsed ? "" : error);
		if (parsed) {
			Check(sli.version == "2.00", "sli: version header is 2.00", "got '" + sli.version + "'");
			Check(sli.labels.size() == 1, "sli: one label", std::to_string(sli.labels.size()) + " labels");
			if (sli.labels.size() == 1)
				Check(sli.labels[0].position == 139776, "sli: Position parsed",
				      std::to_string(sli.labels[0].position));
			Check(!sli.labels.empty() && sli.labels[0].name == "\u30e9\u30d9\u30eb0",
			      "sli: UTF-8 label name survives parsing",
			      sli.labels.empty() ? "" : sli.labels[0].name);
		}
		// ... and re-emit it byte for byte.  This is the assertion that makes
		// the fixed-width column arithmetic in Save() falsifiable.
		Check(sli.Save() == std::string(kRealSliOneLabel), "sli: re-emit is byte-identical to shipped file",
		      std::to_string(sli.Save().size()) + " vs " + std::to_string(std::strlen(kRealSliOneLabel)) + " bytes");
	}

	// (2) Multi-label file: five loop candidates, same column layout.
	{
		SliInfo sli;
		sli.Parse(kRealSliFiveLabels, nullptr);
		const std::vector<int64_t> want = {63488, 206848, 273408, 405504, 455680};
		bool same = sli.labels.size() == want.size();
		for (size_t i = 0; same && i < want.size(); i++) same = sli.labels[i].position == want[i];
		Check(same, "sli: five-label file yields five ordered loop points",
		      std::to_string(sli.labels.size()) + " labels");
		Check(sli.Save() == std::string(kRealSliFiveLabels), "sli: five-label re-emit is byte-identical", "");
	}

	// (3) Unit resolution against the real paired clip.  If Position were
	// milliseconds the loop point would sit 40x past the end of the audio.
	{
		SliInfo sli;
		sli.Parse(kRealSliPaired, nullptr);
		const int64_t maxPos = sli.MaxPosition();
		const int64_t durationMs = kPairedFrames * 1000 / kPairedRate;
		const double samplesRatio = (double)maxPos / (double)kPairedFrames;
		const double millisRatio = (double)maxPos / (double)durationMs;
		char detail[256];
		std::snprintf(detail, sizeof detail,
		              "Position %lld: %.1f%% of clip as samples, %.1fx clip length as ms",
		              (long long)maxPos, samplesRatio * 100.0, millisRatio);
		const SliUnit unit = ResolveSliUnit(sli, kPairedFrames, kPairedRate);
		Check(unit == SliUnit::Samples, "sli: unit resolves to samples on real pair", detail);
		Check(samplesRatio <= 1.0 && millisRatio > 1.0, "sli: milliseconds is arithmetically impossible",
		      "samples fit, ms does not");
		Check(breakIt ? unit != SliUnit::Samples : unit == SliUnit::Samples,
		      "sli: unit verdict is stable", "");
		// A position past the clip must be rejected, which proves the fit test
		// is really comparing numbers rather than defaulting to samples.
		SliInfo absurd;
		absurd.labels.push_back({kPairedFrames * 10, "x"});
		Check(ResolveSliUnit(absurd, kPairedFrames, kPairedRate) == SliUnit::Unknown,
		      "sli: position past end of clip is rejected", "");
		// A-B plan from the real file: intro to the loop point, then [loop,end).
		LoopPlan plan = LoopPlan::FromSli(sli, kPairedFrames, kPairedRate);
		Check(plan.loopStart == maxPos && plan.loopEnd == kPairedFrames && plan.looping,
		      "sli: A-B plan is intro -> loop point -> end",
		      "intro " + std::to_string(plan.introEnd) + ", loop " + std::to_string(plan.loopStart) +
		          ".." + std::to_string(plan.loopEnd));
	}

	std::printf("\n-- Ogg Vorbis decoding (real BGM) --\n");
	const std::string moon = Locate(dataHint, "artemis-nicolan/extracted/bgm/moon.ogg");
	if (moon.empty()) {
		Skip("ogg: container info", "moon.ogg not found (pass --data <testdata dir>)");
	} else {
		std::string error;
		auto source = Source::Open(moon, &error);
		Check(source != nullptr, "ogg: open real moon.ogg", error);
		if (source) {
			const TrackInfo& info = source->Info();
			// Cross-checked against `afinfo`, which reports the same numbers
			// through a different API.
			Check(info.sampleRate == 44100, "ogg: sample rate 44100",
			      std::to_string(info.sampleRate));
			Check(info.channels == 2, "ogg: 2 channels", std::to_string(info.channels));
			const int64_t expected = 7805423 + (breakIt ? 1 : 0);
			Check(info.frames == expected, "ogg: frame count matches afinfo",
			      std::to_string((long long)info.frames) + " vs afinfo " + std::to_string((long long)expected));
			Check(info.codec == "vorb", "ogg: CoreAudio identifies it as vorb", info.codec);
			Check(std::fabs(info.Seconds() - 176.993719) < 0.001, "ogg: duration ~176.99 s",
			      FormatSeconds(info.Seconds()));
		}
	}

	// (4) Decode to PCM and look at the samples, not the file size.
	if (!moon.empty()) {
		std::string error;
		auto source = Source::Open(moon, &error);
		if (source) {
			const TrackInfo& info = source->Info();
			const int64_t rssBefore = ResidentBytes();
			std::vector<float> block((size_t)4096 * info.channels);
			PcmStats stats;
			const int64_t target = (int64_t)(3.0 * info.sampleRate);
			int64_t got = 0;
			const double t0 = NowSeconds();
			while (got < target) {
				const int64_t n = source->Read(block.data(), std::min<int64_t>(4096, target - got));
				if (n <= 0) break;
				stats.Add(block.data(), n, info.channels);
				got += n;
			}
			const double elapsed = NowSeconds() - t0;
			const int64_t rssAfter = ResidentBytes();

			Check(stats.frames == target, "probe: three seconds decode to exactly 132300 frames",
			      std::to_string((long long)stats.frames));
			Check(stats.Rms() > 0.001 && stats.Rms() < 0.5, "probe: RMS in a musical range",
			      std::to_string(stats.Rms()));
			Check(stats.peak <= 1.0 && stats.peak > 0.01, "probe: peak inside (-1,1]",
			      std::to_string(stats.peak));
			Check(stats.SilentFraction() < 0.5, "probe: not mostly silence",
			      std::to_string(stats.SilentFraction() * 100.0) + "% silent");
			Check(stats.ChannelDiffRms() > 1e-6, "probe: stereo channels are genuinely different",
			      std::to_string(stats.ChannelDiffRms()));
			char detail[160];
			std::snprintf(detail, sizeof detail, "%.0fx realtime, rss +%lld B for a %lld B file",
			              3.0 / elapsed, (long long)(rssAfter - rssBefore), (long long)info.fileBytes);
			// Streaming proof: three seconds of a 3.4 MB file must not cost
			// anything like 3.4 MB of memory.
			Check((rssAfter - rssBefore) < info.fileBytes / 2, "probe: streams instead of loading the file", detail);
		}
	}

	// (5) Seek must be repeatable and must actually move.
	if (!moon.empty()) {
		std::string error;
		auto source = Source::Open(moon, &error);
		if (source) {
			const int n = 4096;
			std::vector<float> a((size_t)n * 2), b((size_t)n * 2), c((size_t)n * 2);
			source->Seek(1000000);
			source->Read(a.data(), n);
			source->Seek(1000000);
			source->Read(b.data(), n);
			source->Seek(4000000);
			source->Read(c.data(), n);
			const uint64_t ha = Checksum(a.data(), a.size());
			const uint64_t hb = Checksum(b.data(), b.size());
			const uint64_t hc = Checksum(c.data(), c.size());
			Check(ha == hb, "seek: same frame offset decodes identically twice", "");
			Check(ha != hc, "seek: a different offset yields different audio", "");
		}
	}

	std::printf("\n-- A-B loop and rate conversion --\n");

	// (6) A-B repeat, asserted on the mixed output: with intro [0,10s) and loop
	// [10s,20s), the third ten seconds must equal the second one exactly,
	// because both windows play the same source material.
	if (!moon.empty()) {
		std::string error;
		Stream::Config cfg;
		cfg.engineRate = 44100;
		auto stream = Stream::Open(moon, cfg, &error);
		if (stream) {
			const int rate = 44100;
			LoopPlan plan;
			plan.frames = stream->Info().frames;
			plan.introEnd = 10 * rate;
			plan.loopStart = 10 * rate;
			plan.loopEnd = 20 * rate;
			plan.looping = true;
			stream->Start(plan);
			std::vector<float> out;
			RenderStream(*stream, 30.0, rate, &out);

			const int64_t window = 10 * (int64_t)rate;
			const double loopRepeat = WindowDiff(out, 10 * rate, out, 20 * rate, window);
			const double loopVsIntro = WindowDiff(out, 0, out, 10 * rate, window);
			char detail[160];
			std::snprintf(detail, sizeof detail, "max|loop[10,20) - loop[20,30)| = %.3g", loopRepeat);
			Check(loopRepeat < 1e-6, "loop: A-B window repeats identically", detail);
			std::snprintf(detail, sizeof detail, "max|intro[0,10) - loop[10,20)| = %.3g", loopVsIntro);
			Check(loopVsIntro > 1e-3, "loop: intro differs from loop body (seam actually moved)", detail);
		}
	}

	// (7) Rate conversion: se/kisya.ogg is 48000 Hz while the engine runs at
	// 44100, so the mixer has to resample.  Duration must be preserved.
	{
		const std::string kisya = Locate(dataHint, "artemis-nicolan/extracted/se/kisya.ogg");
		if (kisya.empty()) {
			Skip("resample: 48 kHz effect", "se/kisya.ogg not found");
		} else {
			std::string error;
			Stream::Config cfg;
			cfg.engineRate = 44100;
			auto stream = Stream::Open(kisya, cfg, &error);
			if (!stream) {
				Skip("resample: 48 kHz effect", error);
			} else {
				const TrackInfo& info = stream->Info();
				LoopPlan once = LoopPlan::Whole(info.frames);
				once.looping = false;
				stream->Start(once);
				std::vector<float> block(1024 * 2);
				PcmStats stats;
				int64_t frames = 0;
				const int64_t cap = (int64_t)(info.Seconds() * 44100 * 2 + 44100);
				while (frames < cap) {
					for (int tries = 0; tries < 256 && stream->Buffered() < 1024; tries++)
						if (stream->Fill() == 0 && (stream->Finished() || stream->Stopped())) break;
					std::fill(block.begin(), block.end(), 0.0f);
					const int before = stream->Buffered();
					stream->Mix(block.data(), 1024);
					stats.Add(block.data(), 1024, 2);
					frames += std::min(1024, before);
					if (stream->Finished() && stream->Buffered() == 0) break;
				}
				const double got = (double)frames / 44100.0;
				const double expected = info.Seconds();
				char detail[200];
				std::snprintf(detail, sizeof detail, "%d Hz -> 44100 Hz: %.3f s of %.3f s (%.2f%%)",
				              info.sampleRate, got, expected, 100.0 * got / expected);
				Check(std::fabs(got - expected) / expected < 0.02, "resample: duration preserved across 48k->44.1k", detail);
				Check(stats.Rms() > 0.001, "resample: output is not silence", std::to_string(stats.Rms()));
			}
		}
	}

	std::printf("\n-- Engine --\n");
	if (moon.empty()) {
		Skip("engine: bgm/se/stop", "moon.ogg not found");
	} else {
		std::string dir = moon;
		const size_t slash = dir.find_last_of('/');
		std::string base = dir;
		if (slash != std::string::npos) {
			dir = dir.substr(0, slash);
			base = dir.substr(dir.find_last_of('/') + 1);
		}
		// Point the engine at the extracted tree so names resolve the way the
		// scripts write them ("moon", not "bgm/moon.ogg").
		std::string root = dir;
		if (root.size() > 4 && root.compare(root.size() - 4, 4, "/bgm") == 0) root = root.substr(0, root.size() - 4);

		Engine engine;
		Engine::Config cfg;
		cfg.assetRoot = root;
		cfg.sampleRate = 44100;
		std::string error;
		if (!engine.Open(cfg, &error)) {
			Fail("engine: open", error);
		} else {
			Check(engine.PlayBgm("moon", true, 0), "engine: [bgm file=\"moon\"] plays", base);
			std::vector<float> block(1024 * 2);
			PcmStats bgm;
			for (int i = 0; i < 120; i++) {
				WaitForMix(engine, 1024);
				engine.Mix(block.data(), 1024);
				bgm.Add(block.data(), 1024, 2);
			}
			Check(bgm.Rms() > 0.005, "engine: BGM produces sound", std::to_string(bgm.Rms()));
			Check(engine.ActiveStreams() >= 1, "engine: one stream active",
			      std::to_string(engine.ActiveStreams()));

			// Group volume must actually reach the samples.
			engine.SetVolume(Engine::GroupBgm, 0.0f);
			for (int i = 0; i < 8; i++) { WaitForMix(engine, 1024); engine.Mix(block.data(), 1024); } // settle
			PcmStats muted;
			for (int i = 0; i < 8; i++) {
				WaitForMix(engine, 1024);
				engine.Mix(block.data(), 1024);
				muted.Add(block.data(), 1024, 2);
			}
			Check(muted.Rms() < 1e-4, "engine: SetVolume(bgm,0) silences the group",
			      std::to_string(muted.Rms()));
			engine.SetVolume(Engine::GroupBgm, 1.0f);

			// A sound effect layered on top must raise the peak.
			const double beforePeak = bgm.peak;
			Check(engine.PlaySe("para", -1), "engine: [se file=\"para\"] plays", "");
			PcmStats withSe;
			for (int i = 0; i < 40; i++) {
				WaitForMix(engine, 1024);
				engine.Mix(block.data(), 1024);
				withSe.Add(block.data(), 1024, 2);
			}
			Check(!breakIt ? withSe.peak > beforePeak * 0.5 : withSe.peak < 0, "engine: SE mixes into the same buffer",
			      std::to_string(withSe.peak) + " vs bgm " + std::to_string(beforePeak));

			engine.StopAll();
			for (int i = 0; i < 8; i++) { WaitForMix(engine, 1024); engine.Mix(block.data(), 1024); }
			PcmStats stopped;
			for (int i = 0; i < 8; i++) {
				engine.Mix(block.data(), 1024);
				stopped.Add(block.data(), 1024, 2);
			}
			Check(stopped.peak == 0.0, "engine: StopAll leaves silence", std::to_string(stopped.peak));
			engine.Shutdown();
		}
	}

	RunSecondBatch(dataHint);

	std::printf("\n%d checks, %d failed, %d skipped\n", gChecks, gFailures, gSkips);
	if (gFailures == 0) std::printf("RESULT: OK\n");
	else std::printf("RESULT: FAILED\n");
	return gFailures == 0 ? 0 : 1;
}

// ===========================================================================
// second batch: real device output, mixing behaviour, boundary cases
// ===========================================================================
namespace {

double PercentileOf(std::vector<double> v, double q) {
	if (v.empty()) return 0.0;
	std::sort(v.begin(), v.end());
	const size_t i = (size_t)(q * (double)(v.size() - 1));
	return v[std::min(i, v.size() - 1)];
}

bool FilePresent(const std::string& path) {
	std::FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return false;
	std::fclose(f);
	return true;
}

// Renders `frames` through an Engine, pacing on BufferedFrames() so the decoder
// thread can keep up exactly as a device would.
void RenderEngine(Engine& engine, int frames, std::vector<float>* out) {
	out->assign((size_t)frames * 2, 0.0f);
	const int tick = 512;
	std::vector<float> block((size_t)tick * 2, 0.0f);
	int done = 0;
	while (done < frames) {
		const int n = std::min(tick, frames - done);
		WaitForMix(engine, n);
		engine.Mix(block.data(), n);
		std::memcpy(&(*out)[(size_t)done * 2], block.data(), (size_t)n * 2 * sizeof(float));
		done += n;
	}
}

double WindowRms(const std::vector<float>& v, int startFrame, int frames) {
	double sum = 0.0;
	for (int i = 0; i < frames; i++)
		for (int c = 0; c < 2; c++) {
			const double s = v[(size_t)((startFrame + i) * 2 + c)];
			sum += s * s;
		}
	return frames ? std::sqrt(sum / ((double)frames * 2.0)) : 0.0;
}

// Largest jump between neighbouring samples anywhere in a buffer: the objective
// measure of a "click".
double MaxAdjacentJump(const std::vector<float>& v) {
	double worst = 0.0;
	for (size_t i = 2; i < v.size(); i += 2) {
		const double d = std::fabs((double)v[i] - (double)v[i - 2]);
		if (d > worst) worst = d;
	}
	return worst;
}

// Writes a sine tone to an Ogg Vorbis file with ffmpeg.  Only used for cases the
// shipped data does not cover (a clip shorter than 100 ms, and a bank of eight
// distinct effects); everything else uses real game audio.
bool MakeTone(const std::string& path, double seconds, double hertz) {
	char command[1024];
	std::snprintf(command, sizeof command,
	              "/opt/homebrew/bin/ffmpeg -y -loglevel error -f lavfi -i "
	              "\"sine=frequency=%.1f:duration=%.3f\" -af volume=4.0 -ar 44100 -ac 2 -c:a vorbis -strict -2 \"%s\" "
	              ">/dev/null 2>&1",
	              hertz, seconds, path.c_str());
	if (std::system(command) != 0) return false;
	return FilePresent(path);
}

} // namespace

int RunSecondBatch(const std::string& dataHint) {
	const bool breakIt = std::getenv("AUDIOTOOL_SELFTEST_BREAK") != nullptr;
	const std::string moon = Locate(dataHint, "artemis-nicolan/extracted/bgm/moon.ogg");
	const std::string niji = Locate(dataHint, "artemis-nicolan/extracted/bgm/niji.ogg");
	const std::string mono = Locate(dataHint, "artemis-nicolan/extracted/se/biyobiyo.ogg");
	const std::string kisya = Locate(dataHint, "artemis-nicolan/extracted/se/kisya.ogg");
	std::string root = moon;
	const size_t lastSlash = root.find_last_of('/');
	if (lastSlash != std::string::npos) root = root.substr(0, lastSlash);
	if (root.size() > 4 && root.compare(root.size() - 4, 4, "/bgm") == 0) root = root.substr(0, root.size() - 4);

	std::printf("\n-- real device output --\n");
	{
		const DeviceList list = Device::Enumerate();
		std::printf("  devices with output channels: %zu\n", list.devices.size());
		for (size_t i = 0; i < list.devices.size(); i++)
			std::printf("    [%zu]%s %-28s %d ch  %.0f Hz\n", i,
			            (int)i == list.defaultIndex ? " *" : "  ", list.devices[i].name.c_str(),
			            list.devices[i].outputChannels, list.devices[i].sampleRate);
		if (list.devices.empty())
			Skip("device: enumeration finds an output", "none reported");
		else
			Pass("device: enumeration finds an output", std::to_string(list.devices.size()) + " device(s)");

		const DeviceTarget target = Device::QueryDefault();
		if (!target.available) {
			// A headless box is a legitimate place to run this: say why, stay green.
			Skip("device: open default output", target.reason);
		} else {
			std::printf("  default: %s  %.0f Hz  %d ch  buffer %d frames\n", target.name.c_str(),
			            target.sampleRate, target.channels, target.bufferFrames);
			Engine engine;
			Engine::Config cfg;
			cfg.assetRoot = root;
			cfg.sampleRate = (int)target.sampleRate; // mixer runs at the device rate
			std::string error;
			if (!engine.Open(cfg, &error)) {
				Fail("device: engine opens at device rate", error);
			} else {
				const bool played = moon.empty() ? false : engine.PlayBgm("moon", true, 0);
				Device device;
				std::string reason;
				if (!device.Open(&engine, target, &reason)) {
					Skip("device: open default output", reason);
				} else if (!device.Start(&reason)) {
					Fail("device: start", reason);
				} else {
					Pass("device: open default output",
					     target.name + " @ " + std::to_string((int)target.sampleRate) + " Hz, buffer " +
					         std::to_string(device.BufferFrames()) + " frames");
					// Let it run on real hardware.
					std::this_thread::sleep_for(std::chrono::milliseconds(1500));
					device.RefreshStats();
					const CallbackStats st = device.Stats();
					const double periodMs = 1000.0 * (double)device.BufferFrames() / target.sampleRate;
					char detail[320];
					std::snprintf(detail, sizeof detail,
					              "%llu callbacks, %llu frames, interval p50 %.2f ms p99 %.2f ms max %.2f ms "
					              "(jitter %.3f ms), underruns %llu",
					              (unsigned long long)st.callbacks, (unsigned long long)st.frames, st.intervalP50Ms,
					              st.intervalP99Ms, st.intervalMaxMs, st.intervalJitterMs,
					              (unsigned long long)st.underruns);
					Check(st.callbacks > 0, "device: callback fired on real hardware", detail);
					std::snprintf(detail, sizeof detail, "work p50 %.1f us, p99 %.1f us, max %.1f us (period %.2f ms)",
					              st.workP50Us, st.workP99Us, st.workMaxUs, periodMs);
					Check(st.workP99Us < periodMs * 1000.0 * 0.5,
					      "device: mix work uses under half the callback period", detail);
					Check(!breakIt ? st.underruns == 0 : st.underruns > 0,
					      "device: no underruns while playing", std::to_string((long long)st.underruns));
					Check(st.frames > 0 && played, "device: audio was actually fed to the device", "");
					device.Stop();
					device.Close();
				}
				engine.Shutdown();
			}
		}
	}

	std::printf("\n-- realtime safety --\n");
	{
		// The real callback cannot audit itself without allocating, so run the same
		// mix body on a normal thread and count heap blocks across the call.
		if (moon.empty()) {
			Skip("rt: mixer allocates nothing", "moon.ogg missing");
		} else {
			Engine engine;
			Engine::Config cfg;
			cfg.assetRoot = root;
			cfg.sampleRate = 44100;
			std::string error;
			engine.Open(cfg, &error);
			engine.PlayBgm("moon", true, 0);
			WaitForMix(engine, 512);
			int worstIteration = -1;
			const int64_t worst = Device::AuditMixerAllocations(&engine, 512, 400, &worstIteration);
			char detail[160];
			std::snprintf(detail, sizeof detail, "%lld heap blocks allocated over 400 Mix() calls", (long long)worst);
			Check(worst == 0, "rt: mixer allocates nothing", detail);
			engine.Shutdown();
		}
	}

	std::printf("\n-- mixing behaviour --\n");

	// (A) Loop seam: the engine must not add a discontinuity of its own.  The
	// seam jump of the output has to equal the source's own jump between the
	// loop point and the last frame before it.
	if (!moon.empty()) {
		std::string error;
		Stream::Config cfg;
		cfg.engineRate = 44100;
		auto stream = Stream::Open(moon, cfg, &error);
		if (stream) {
			const int rate = 44100;
			LoopPlan plan;
			plan.frames = stream->Info().frames;
			plan.introEnd = 10 * rate;
			plan.loopStart = 10 * rate;
			plan.loopEnd = 20 * rate;
			plan.looping = true;
			stream->Start(plan);
			std::vector<float> out;
			RenderStream(*stream, 21.0, rate, &out);
			// The seam lands exactly at t = 20 s in the output.
			const int seamFrame = 20 * rate;
			const double jumpOut = std::fabs((double)out[(size_t)seamFrame * 2] - (double)out[(size_t)(seamFrame - 1) * 2]);
			// The material's own jump: last frame of the loop body vs the loop point.
			// Decoded sequentially on purpose - seeking to a frame lands on a packet
			// boundary (measured +896 frames) and would read the wrong sample.
			std::string err2;
			auto ref = Source::Open(moon, &err2);
			std::vector<float> decoded((size_t)(20 * rate) * 2, 0.0f);
			const int64_t have = ref ? ref->Read(decoded.data(), 20 * rate) : 0;
			const double jumpSource =
			    have >= 20 * rate ? std::fabs((double)decoded[(size_t)(10 * rate) * 2] -
			                                  (double)decoded[(size_t)(20 * rate - 1) * 2])
			                      : -1.0;
			char detail[220];
			std::snprintf(detail, sizeof detail, "output jump %.4f vs material's own jump %.4f",
			              jumpOut, jumpSource);
			// Not "below 0.05" on arbitrary material - that would be cherry picking.
			// What the engine owes is that it adds nothing of its own.
			Check(std::fabs(jumpOut - jumpSource) < 1e-4, "loop: seam jump is the material's own, not a glitch", detail);
			Check(MaxAdjacentJump(out) < 0.2, "loop: no large click anywhere in 21 s of looping output",
			      std::to_string(MaxAdjacentJump(out)));
		}
	}

	// (B) Eight concurrent effects must not clip.
	{
		const std::string dir = "/tmp/artemis_audio_bank";
		std::system(("mkdir -p " + dir).c_str());
		// The real BGM goes beside the synthesised effects so a single asset root
		// can produce the "BGM + 8 effects" case.  Copy only; the game data is
		// read-only.
		if (!moon.empty()) std::system(("cp -f \"" + moon + "\" \"" + dir + "/moon.ogg\"").c_str());
		bool bank = true;
		for (int i = 0; i < 8; i++)
			bank = bank && MakeTone(dir + "/se" + std::to_string(i) + ".ogg", 1.2, 440.0);
		if (!bank) {
			Skip("mix: eight concurrent effects", "ffmpeg could not synthesise a test bank");
		} else {
			Engine engine;
			Engine::Config cfg;
			cfg.assetRoot = dir;
			cfg.sampleRate = 44100;
			std::string error;
			engine.Open(cfg, &error);
			engine.SetVolume(Engine::GroupSe, 1.0f);
			int started = 0;
			for (int i = 0; i < 8; i++)
				if (engine.PlaySe("se" + std::to_string(i), i)) started++;
			std::vector<float> out;
			RenderEngine(engine, 44100 / 2, &out);
			double peak = 0.0;
			int64_t atFullScale = 0;
			for (float v : out) {
				const double a = std::fabs((double)v);
				if (a > peak) peak = a;
				if (a >= 0.9999) atFullScale++;
			}
			char detail[240];
			std::snprintf(detail, sizeof detail, "%d effects, peak %.4f, %lld samples at full scale",
			              started, peak, (long long)atFullScale);
			Check(started == 8, "mix: eight effects started concurrently", detail);
			Check(peak <= 1.0, "mix: output never exceeds full scale", detail);
			// A hard clipper would leave a flat top at exactly 1.0; the soft limiter
			// never reaches it.  This is what distinguishes limiting from clipping.
			Check(atFullScale == 0, "mix: no flat-topped clipping", detail);
			Check(peak > kLimiterKnee, "mix: limiter actually engaged on the pile-up", detail);

			// The limiter must be transparent below the knee.
			bool linear = true;
			for (float x = -kLimiterKnee; x <= kLimiterKnee; x += 0.01f)
				if (SoftLimit(x) != x) linear = false;
			Check(linear, "mix: limiter is bit transparent below the knee", "");
			Check(std::fabs(SoftLimit(8.0f)) < 1.0f && std::fabs(SoftLimit(-8.0f)) < 1.0f,
			      "mix: limiter bounds even absurd input", "");
			engine.Shutdown();
		}
	}

	// (C) Crossfade, sample by sample: the outgoing track has to keep sounding
	// while it fades, so there is no hole in the middle.
	if (!moon.empty() && !niji.empty()) {
		Engine engine;
		Engine::Config cfg;
		cfg.assetRoot = root;
		cfg.sampleRate = 44100;
		std::string error;
		engine.Open(cfg, &error);
		// Headroom so the two tracks sum linearly and the limiter stays out of it.
		engine.SetVolume(Engine::GroupBgm, 0.35f);

		std::vector<float> before, during;
		engine.PlayBgm("moon", true, 0);
		RenderEngine(engine, 44100, &before);
		const bool switched = engine.PlayBgm("niji", true, 1000);
		const int streamsDuringFade = engine.ActiveStreams();
		RenderEngine(engine, 44100 * 2, &during);

		// niji alone, with the same 1 s fade-in, so subtracting it leaves exactly the
		// outgoing track's tail.
		Engine solo;
		solo.Open(cfg, &error);
		solo.SetVolume(Engine::GroupBgm, 0.35f);
		solo.PlayBgm("niji", true, 1000);
		std::vector<float> reference;
		RenderEngine(solo, 44100 * 2, &reference);

		// After two seconds the outgoing track must have gone and released its slot.
		const int streamsAfterFade = engine.ActiveStreams();


		const int rate = 44100;
		const double rmsBefore = WindowRms(before, 0, rate);
		const double rmsSolo = WindowRms(reference, 0, rate);
		const double quietest = std::min(WindowRms(during, 0, rate / 8), WindowRms(during, rate - rate / 8, rate / 8));
		char detail[300];
		Check(switched, "crossfade: second BGM starts", "");
		Check(streamsDuringFade == 2, "crossfade: outgoing track keeps its own slot",
		      std::to_string(streamsDuringFade) + " streams during the fade");
		Check(streamsAfterFade == 1, "crossfade: slot is released once silent",
		      std::to_string(streamsAfterFade) + " streams afterwards");
		std::snprintf(detail, sizeof detail, "quietest window rms %.5f vs tracks %.5f / %.5f", quietest, rmsBefore,
		              rmsSolo);
		Check(quietest > 0.25 * std::min(rmsBefore, rmsSolo), "crossfade: no hole mid-fade (both tracks overlap)",
		      detail);

		// The envelope itself, measured window by window.  Two streams of the same
		// file are advanced in lockstep; one fades, the other does not, so the ratio
		// of their energies IS the gain curve.  This is the sample-accurate
		// statement "the old track fades to silence and the new one fades up".
		{
			Stream::Config sc;
			sc.engineRate = 44100;
			std::string err3;
			auto fadeOut = Stream::Open(moon, sc, &err3);
			auto steadyOut = Stream::Open(moon, sc, &err3);
			auto fadeIn = Stream::Open(moon, sc, &err3);
			auto steadyIn = Stream::Open(moon, sc, &err3);
			if (fadeOut && steadyOut && fadeIn && steadyIn) {
				LoopPlan lp = LoopPlan::Whole(fadeOut->Info().frames);
				fadeOut->Start(lp);
				steadyOut->Start(lp);
				fadeIn->Start(lp);
				steadyIn->Start(lp);
				std::vector<float> discard;
				RenderStream(*fadeOut, 0.25, rate, &discard);
				RenderStream(*steadyOut, 0.25, rate, &discard);
				RenderStream(*fadeIn, 0.25, rate, &discard);
				RenderStream(*steadyIn, 0.25, rate, &discard);

				fadeOut->FadeFromTo(1.0f, 0.0f, 500);
				fadeIn->FadeFromTo(0.0f, 1.0f, 500);
				std::vector<float> outFading, outSteady, inFading, inSteady;
				RenderStream(*fadeOut, 0.5, rate, &outFading);
				RenderStream(*steadyOut, 0.5, rate, &outSteady);
				RenderStream(*fadeIn, 0.5, rate, &inFading);
				RenderStream(*steadyIn, 0.5, rate, &inSteady);

				const int span = (int)(outFading.size() / 2);
				const int windows = 8;
				const int win = span / windows;
				double down[8], up[8];
				for (int k = 0; k < windows; k++) {
					const double ref = WindowRms(outSteady, k * win, win);
					const double got = WindowRms(outFading, k * win, win);
					down[k] = ref > 1e-9 ? got / ref : 0.0;
					const double refIn = WindowRms(inSteady, k * win, win);
					const double gotIn = WindowRms(inFading, k * win, win);
					up[k] = refIn > 1e-9 ? gotIn / refIn : 0.0;
				}
				bool monotoneDown = true, monotoneUp = true;
				for (int k = 1; k < windows; k++) {
					if (down[k] > down[k - 1] + 0.08) monotoneDown = false;
					if (up[k] < up[k - 1] - 0.08) monotoneUp = false;
				}
				std::snprintf(detail, sizeof detail, "gain %.2f -> %.2f over 500 ms, 8 windows", down[0], down[7]);
				Check(monotoneDown && down[0] > 0.6 && down[7] < 0.15,
				      "crossfade: outgoing gain curve falls to silence", detail);
				std::snprintf(detail, sizeof detail, "gain %.2f -> %.2f over 500 ms, 8 windows", up[0], up[7]);
				Check(monotoneUp && up[0] < 0.25 && up[7] > 0.75, "crossfade: incoming gain curve rises from silence",
				      detail);
			}
		}
		engine.Shutdown();
		solo.Shutdown();
	}

	// (D) Group volumes are independent.
	if (!moon.empty()) {
		Engine engine;
		Engine::Config cfg;
		cfg.assetRoot = root;
		cfg.sampleRate = 44100;
		std::string error;
		engine.Open(cfg, &error);
		engine.PlayBgm("moon", true, 0);
		WaitForMix(engine, 512);
		engine.SetVolume(Engine::GroupSe, 0.0f);
		std::vector<float> out;
		RenderEngine(engine, 44100 * 2, &out);
		// Second second, not the first: moon.ogg fades in from near silence.
		const double rmsWithSeMuted = WindowRms(out, 44100, 44100);
		Check(rmsWithSeMuted > 0.001, "volume: muting the SE group does not touch BGM",
		      std::to_string(rmsWithSeMuted));
		Check(engine.Volume(Engine::GroupSe) == 0.0f && engine.Volume(Engine::GroupBgm) == 1.0f,
		      "volume: groups hold independent values", "");
		engine.Shutdown();
	}

	std::printf("\n-- boundary cases --\n");
	{
		// Ultra-short clip: generated, because none of the shipped effects is
		// shorter than a second.
		const std::string tiny = "/tmp/artemis_audio_tiny.ogg";
		if (!MakeTone(tiny, 0.06, 880.0)) {
			Skip("edge: clip shorter than 100 ms", "ffmpeg unavailable");
		} else {
			std::string error;
			auto source = Source::Open(tiny, &error);
			Check(source != nullptr, "edge: open a 60 ms clip", error);
			if (source) {
				const TrackInfo& info = source->Info();
				Check(info.frames > 0 && info.Seconds() < 0.1, "edge: 60 ms clip reports a sub-100 ms duration",
				      FormatSeconds(info.Seconds()));
				for (bool loop : {false, true}) {
					Stream::Config c;
					c.engineRate = 44100;
					auto stream = Stream::Open(tiny, c, &error);
					LoopPlan plan = LoopPlan::Whole(info.frames);
					plan.looping = loop;
					stream->Start(plan);
					std::vector<float> out;
					RenderStream(*stream, 0.5, 44100, &out);
					const double rms = WindowRms(out, 0, (int)(out.size() / 2));
					char detail[200];
					std::snprintf(detail, sizeof detail, "%s: rms %.5f over 500 ms of output",
					              loop ? "looping" : "one shot", rms);
					Check(rms > 0.001, loop ? "edge: sub-100 ms clip loops without falling silent"
					                       : "edge: sub-100 ms clip plays as a one shot",
					      detail);
				}
			}
		}
	}

	// Mono -> stereo: the same sample must appear on both channels, exactly.
	if (!mono.empty()) {
		std::string error;
		auto source = Source::Open(mono, &error);
		Check(source && source->Info().channels == 1, "edge: real effect is mono",
		      source ? std::to_string(source->Info().channels) + " ch" : error);
		Stream::Config c;
		c.engineRate = 44100;
		auto stream = Stream::Open(mono, c, &error);
		if (stream) {
			LoopPlan once = LoopPlan::Whole(stream->Info().frames);
			once.looping = false;
			stream->Start(once);
			std::vector<float> out;
			RenderStream(*stream, 0.5, 44100, &out);
			double worst = 0.0;
			for (size_t i = 0; i < out.size(); i += 2) worst = std::max(worst, std::fabs((double)out[i] - out[i + 1]));
			Check(worst == 0.0, "edge: mono is upmixed to identical channels", std::to_string(worst));
			const double monoRms = WindowRms(out, 0, (int)(out.size() / 2));
			Check(monoRms > 0.001, "edge: mono upmix is audible", std::to_string(monoRms));
		}
	}

	// 48 kHz effect into a 44.1 kHz mixer: duration must be preserved.
	if (!kisya.empty()) {
		std::string error;
		auto source = Source::Open(kisya, &error);
		Check(source && source->Info().sampleRate == 48000, "edge: real effect is 48 kHz",
		      source ? std::to_string(source->Info().sampleRate) + " Hz" : error);
	}

	// .sli loop point at the very start and at the very end.
	{
		const int64_t frames = 44100 * 3;
		SliInfo atStart;
		atStart.version = "2.00";
		atStart.labels.push_back({0, "zero"});
		atStart.unit = SliUnit::Samples;
		LoopPlan p0 = LoopPlan::FromSli(atStart, frames, 44100);
		Check(p0.loopStart == 0 && p0.loopEnd == frames && p0.looping,
		      "edge: loop point 0 loops the whole file", std::to_string((long long)p0.loopStart));

		SliInfo atEnd;
		atEnd.version = "2.00";
		atEnd.labels.push_back({frames, "end"});
		atEnd.unit = SliUnit::Samples;
		LoopPlan pe = LoopPlan::FromSli(atEnd, frames, 44100);
		Check(pe.loopStart == 0 && pe.loopEnd == frames,
		      "edge: loop point at the end degrades to whole-file loop", "");

		SliInfo past;
		past.version = "2.00";
		past.labels.push_back({frames * 4, "past"});
		past.unit = SliUnit::Samples;
		LoopPlan pp = LoopPlan::FromSli(past, frames, 44100);
		Check(pp.loopStart >= 0 && pp.loopEnd == frames && pp.loopEnd > pp.loopStart,
		      "edge: loop point past the end is clamped safely", "");
	}

	// (multi-label .sli covered by the shipped fixture earlier; here check the
	// rule directly on a synthetic three-label file)
	{
		SliInfo multi;
		multi.version = "2.00";
		multi.labels.push_back({44100, "one"});
		multi.labels.push_back({88200, "two"});
		multi.labels.push_back({132300, "three"});
		multi.unit = SliUnit::Samples;
		LoopPlan p = LoopPlan::FromSli(multi, 44100 * 10, 44100);
		Check(p.loopStart == 132300, "edge: multiple labels take the last one",
		      std::to_string((long long)p.loopStart));
	}

	// Damaged files must be refused, not crash the process.
	{
		bool refused = true;
		for (int variant = 0; variant < 3; variant++) {
			const std::string path = "/tmp/artemis_audio_broken" + std::to_string(variant) + ".ogg";
			std::FILE* out = std::fopen(path.c_str(), "wb");
			if (!out) {
				refused = false;
				break;
			}
			if (variant == 0) {
				const char junk[] = "not an ogg file at all, just bytes";
				std::fwrite(junk, 1, sizeof junk, out);
			} else if (variant == 1) {
				// A real Ogg page header with nothing behind it.
				const unsigned char head[] = {'O', 'g', 'g', 'S', 0, 2, 0, 0, 0, 0, 0, 0, 0, 0};
				std::fwrite(head, 1, sizeof head, out);
			} else if (!moon.empty()) {
				// A truncated real file: 200 bytes of a 3.4 MB track.
				std::FILE* in = std::fopen(moon.c_str(), "rb");
				char buf[200];
				const size_t n = in ? std::fread(buf, 1, sizeof buf, in) : 0;
				if (in) std::fclose(in);
				std::fwrite(buf, 1, n, out);
			}
			std::fclose(out);

			std::string error;
			auto source = Source::Open(path, &error);
			if (source) {
				// Opening may succeed if the header survived; decoding must then
				// return nothing rather than garbage or a crash.
				std::vector<float> probe((size_t)1024 * 2);
				const int64_t got = source->Read(probe.data(), 1024);
				if (got > 512) refused = false;
			}
		}
		Check(refused, "edge: damaged ogg files are refused without crashing", "3 variants");
	}

	std::printf("\n-- performance and resources --\n");
	{
		if (moon.empty()) {
			Skip("perf: callback timing distribution", "moon.ogg missing");
		} else {
			Engine engine;
			Engine::Config cfg;
			cfg.assetRoot = root;
			cfg.sampleRate = 44100;
			std::string error;
			engine.Open(cfg, &error);
			engine.PlayBgm("moon", true, 0);
			WaitForMix(engine, 512);

			const int64_t rssBefore = ResidentBytes();
			std::vector<float> block(512 * 2, 0.0f);
			std::vector<double> workUs;
			workUs.reserve(4000);
			const double t0 = NowSeconds();
			for (int i = 0; i < 4000; i++) {
				WaitForMix(engine, 512);
				const double a = NowSeconds();
				engine.Mix(block.data(), 512);
				workUs.push_back((NowSeconds() - a) * 1e6);
			}
			const double elapsed = NowSeconds() - t0;
			const int64_t rssAfter = ResidentBytes();
			const double audioSeconds = (double)(4000 * 512) / 44100.0;
			const double periodMs = 1000.0 * 512.0 / 44100.0;
			char detail[320];
			std::snprintf(detail, sizeof detail,
			              "p50 %.1f us, p99 %.1f us, max %.1f us for a %.2f ms period (%.0fx realtime)",
			              PercentileOf(workUs, 0.50), PercentileOf(workUs, 0.99), PercentileOf(workUs, 1.0),
			              periodMs, audioSeconds / elapsed);
			Check(PercentileOf(workUs, 0.99) < periodMs * 1000.0 * 0.5,
			      "perf: mix callback p99 well inside the period", detail);
			std::snprintf(detail, sizeof detail, "%s before, %s after %.2f s of mixing", HumanBytes(rssBefore).c_str(),
			              HumanBytes(rssAfter).c_str(), audioSeconds);
			Check(rssAfter - rssBefore < 4 * 1024 * 1024, "perf: resident memory stays flat", detail);
			engine.Shutdown();
		}

		// Eight effects plus BGM: how much CPU does that cost?
		const std::string dir = "/tmp/artemis_audio_bank";
		if (!FilePresent(dir + "/se7.ogg")) {
			Skip("perf: eight effects CPU", "test bank missing");
		} else {
			Engine engine;
			Engine::Config cfg;
			cfg.assetRoot = dir; // holds the real moon.ogg plus se0..se7
			cfg.sampleRate = 44100;
			std::string error;
			engine.Open(cfg, &error);
			if (!moon.empty()) engine.PlayBgm("moon", true, 0);
			int started = 0;
			for (int i = 0; i < 8; i++)
				if (engine.PlaySe("se" + std::to_string(i), i)) started++;
			if (started < 8) {
				Skip("perf: eight effects CPU", std::to_string(started) + " effects could not start");
			} else {
				const int frames = 44100 * 4;
				const double t0 = NowSeconds();
				std::vector<float> out;
				RenderEngine(engine, frames, &out);
				const double elapsed = NowSeconds() - t0;
				const double audioSeconds = (double)frames / 44100.0;
				char detail[200];
				std::snprintf(detail, sizeof detail, "%d effects + BGM rendered %.1f s of audio in %.3f s (%.0fx realtime)",
				              started, audioSeconds, elapsed, audioSeconds / elapsed);
				Check(audioSeconds / elapsed > 20.0, "perf: nine concurrent streams still far faster than realtime",
				      detail);
				engine.Shutdown();
			}
		}
	}

	return 0;
}

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr,
		             "usage: audiotool <command> [args]\n"
		             "  info  <file.ogg>\n"
		             "  probe <file.ogg> [--seconds N]\n"
		             "  sli   <file.sli> [--ogg <file.ogg>]\n"
		             "  sli-batch <dir>\n"
		             "  play  <file.ogg> [--loop] [--seconds N] [--se NAME]\n"
		             "  devices\n"
		             "  device <file.ogg> [--loop] [--seconds N] [--se NAME]\n"
		             "  selftest [--data <dir>]\n");
		return 2;
	}
	std::string command = argv[1];
	double seconds = 0.0;
	std::string ogg, se, dataDir;
	int64_t givenFrames = 0;
	int givenRate = 0;
	bool loop = false;
	for (int i = 2; i < argc; i++) {
		std::string arg = argv[i];
		if (arg == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
		else if (arg == "--ogg" && i + 1 < argc) ogg = argv[++i];
		else if (arg == "--se" && i + 1 < argc) se = argv[++i];
		else if (arg == "--data" && i + 1 < argc) dataDir = argv[++i];
		else if (arg == "--frames" && i + 1 < argc) givenFrames = std::atoll(argv[++i]);
		else if (arg == "--rate" && i + 1 < argc) givenRate = std::atoi(argv[++i]);
		else if (arg == "--loop") loop = true;
	}

	if (command == "selftest") return RunSelfTest(dataDir);
	if (command == "devices") return CommandDevices();

	std::string target;
	for (int i = 2; i < argc; i++) {
		std::string arg = argv[i];
		if (!arg.empty() && arg[0] == '-') {
			if (arg == "--seconds" || arg == "--ogg" || arg == "--se" || arg == "--data" ||
			    arg == "--frames" || arg == "--rate") i++;
			continue;
		}
		target = arg;
		break;
	}
	if (target.empty()) {
		std::fprintf(stderr, "%s: need a file argument\n", command.c_str());
		return 2;
	}

	if (command == "info") return CommandInfo(target);
	if (command == "probe") return CommandProbe(target, seconds > 0 ? seconds : 5.0);
	if (command == "sli") return CommandSli(target, ogg, givenFrames, givenRate);
	if (command == "sli-batch") return CommandSliBatch(target);
	if (command == "play") return CommandPlay(target, loop, seconds > 0 ? seconds : 3.0, se);
	if (command == "device") return CommandDevice(target, loop, seconds > 0 ? seconds : 3.0, se);
	std::fprintf(stderr, "unknown command: %s\n", command.c_str());
	return 2;
}
