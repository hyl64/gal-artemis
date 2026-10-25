// movie.h - video playback for the `movie` script tag
//
// Five real scripts run `{"movie", cancel=1, file="op"}` and friends (see
// docs: the tags are `{"movie", ...}` Lua tables, not KAG `[...]` tags).  To
// play them the engine has to turn an mp4/avi into RGBA frames it can composite,
// which is what this layer does through AVFoundation.
//
// Two things shape the design:
//
//   * **AVFoundation is Objective-C.**  The project builds pure C++ targets, so
//     movie.cpp is compiled as Objective-C++ (CMake passes `-x objective-c++`
//     for src/movie/*.cpp).  This header stays plain C++ so the renderer and the
//     tools include it without dragging the Objective-C runtime along.
//
//   * **Frames must not be buffered.**  A 2 minute 720p clip is gigabytes of
//     raw RGBA, so the decoder hands out one frame at a time and never keeps a
//     film queue.  Any seek restarts an AVAssetReader, which is the only way to
//     reposition one; that cost is measured rather than hidden.
//
// The renderer drives this with a wall clock:
//
//     MovieLayer layer;
//     layer.Play("movie/op.mp4", /*clearToBlack*/ true, &error);   // bg="black"
//     ...each frame...
//     if (layer.Advance(elapsedMs, &canvas)) blit(canvas);          // cancel=1 -> layer.Cancel()
//
// Which is the whole "one line to hook into agame" story: the player already
// knows how many milliseconds have passed, because it drives the rest of the
// timeline the same way.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace artemis
{
namespace movie
{

// An RGBA8 frame, straight (non-premultiplied) alpha, as the compositor wants it.
struct Canvas
{
	int width = 0;
	int height = 0;
	std::vector<uint8_t> rgba; // width * height * 4

	void Resize(int w, int h);
	bool Empty() const { return width <= 0 || height <= 0 || rgba.empty(); }

	// Mean luma (Rec.601) over the frame, 0..255.  The cheap "is this actually a
	// picture, and is it the picture I expect" number the probes print.
	double MeanLuma() const;
	// Mean of each channel, 0..255.  Coloured test clips are asserted on this.
	void MeanRgb(double* r, double* g, double* b) const;
	// Fill with opaque black; used for `bg="black"` before the first frame.
	void FillBlack();
};

struct MovieInfo
{
	int width = 0;
	int height = 0;
	double fps = 0.0;
	int64_t frameCount = 0;
	int64_t durationMs = 0;
	bool hasAudio = false;
	int audioChannels = 0;
	double audioSampleRate = 0.0;
	std::string videoCodec; // fourcc, e.g. "avc1"
	std::string audioCodec;
};

// One decoded movie file.  Not thread safe: give each playing movie its own.
class Movie
{
public:
	static std::unique_ptr<Movie> Open(const std::string& path, std::string* error);
	~Movie();
	Movie(const Movie&) = delete;
	Movie& operator=(const Movie&) = delete;

	const MovieInfo& Info() const { return info_; }

	// Decodes the next frame in presentation order into `out`.
	// Returns false at end of file or on error; `error` distinguishes the two.
	bool ReadNext(Canvas* out, std::string* error);

	// Repositions to the frame that covers `ms`, so the next ReadNext returns it.
	bool Seek(int64_t ms, std::string* error);

	// Frame index of the frame covering `ms`.
	int64_t FrameIndexAt(int64_t ms) const;

	// Convenience: seek then read one frame.
	bool FrameAt(int64_t ms, Canvas* out, std::string* error);

	int64_t PositionMs() const { return positionMs_; }
	int64_t FrameIndex() const { return frameIndex_; }

private:
	Movie() = default;
	struct Impl;
	std::unique_ptr<Impl> impl_;
	MovieInfo info_;
	int64_t positionMs_ = 0;
	int64_t frameIndex_ = 0;
};

// The playing layer a renderer actually holds.  Owns at most one movie, tracks
// how far the timeline has advanced, and understands the two tag attributes that
// matter: `cancel=1` (stop early) and `bg="black"` (show black underneath).
class MovieLayer
{
public:
	// `path` is the file the script named.  `clearToBlack` makes the layer report
	// an opaque black canvas until the first frame is decoded, which is what
	// bg="black" asks the player to draw behind the video.
	bool Play(const std::string& path, bool clearToBlack, std::string* error);

	// Advances to wall-clock `elapsedMs` since Play().  Fills `out` and returns
	// true while the movie is on screen.  Returns false once it has finished
	// (the caller then falls back to whatever the script drew next).
	bool Advance(int64_t elapsedMs, Canvas* out);

	// `cancel=1`: stop immediately and forget the movie.
	void Cancel();

	bool Active() const { return movie_ != nullptr; }
	bool Finished() const { return finished_; }
	bool ClearedToBlack() const { return clearToBlack_; }
	// The colour the layer draws \eunder the video.  `bg="black"` asks for opaque
	// black at the movie's own size, which the player blits before the frame.
	const Canvas& Backdrop() const { return backdrop_; }
	const MovieInfo& Info() const { return info_; }
	int64_t FramesShown() const { return framesShown_; }
	double AverageSeekMs() const { return seekCount_ ? seekTotalMs_ / (double)seekCount_ : 0.0; }
	int64_t SeekCount() const { return seekCount_; }

private:
	std::unique_ptr<Movie> movie_;
	MovieInfo info_;
	bool clearToBlack_ = false;
	Canvas backdrop_;
	bool finished_ = false;
	int64_t currentFrame_ = -1;
	int64_t framesShown_ = 0;
	int64_t seekCount_ = 0;
	double seekTotalMs_ = 0.0;
};

// Decode one movie end to end and report what came out.  Used by `movietool
// probe` and by the self-test; keeps no frames.
struct ProbeResult
{
	int64_t framesDecoded = 0;
	int64_t firstFrameLuma = 0;
	int64_t lastFrameLuma = 0;
	double firstR = 0, firstG = 0, firstB = 0;
	double lastR = 0, lastG = 0, lastB = 0;
	double minLuma = 0, maxLuma = 0;
	double lumaVariance = 0.0;
	double secondsWall = 0.0;
	int64_t peakRssDeltaBytes = 0;
};

bool Probe(const std::string& path, ProbeResult* result, std::string* error);

// Wall clock / memory helpers, same conventions as the audio layer.
double NowSeconds();
int64_t ResidentBytes();

} // namespace movie
} // namespace artemis
