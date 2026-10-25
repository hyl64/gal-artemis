// movie.cpp - AVFoundation decode path for the `movie` tag
//
// Compiled as Objective-C++ (CMake passes -x objective-c++ -fobjc-arc for
// src/movie/*.cpp), because AVFoundation has no C API.  Everything Objective-C
// stays inside this one file; movie.h is plain C++.
//
// How frames are chosen, which is the part that needs care:
//
//   A file's frame timestamps are *not* guaranteed to be evenly spaced.  CFR is
//   the common case and VFR really happens (edited trailers, phone captures), so
//   nothing here assumes frame N sits at N/fps.  Instead every decoded frame
//   carries its real presentation time, and FrameAt(ms) returns the last frame
//   whose timestamp is <= ms - the frame that is actually on screen at that
//   moment.  fps is only used as a scheduling hint for the player, never to
//   decide *which* frame is correct.
//
//   Repositioning needs a fresh AVAssetReader (an AVAssetReader cannot seek), so
//   a seek decodes forward from a little before the target rather than from the
//   previous keyframe on its own.  That seek cost is measured, not hidden.
//
// Memory: exactly one frame is held at a time, so resident size is one frame and
// a few buffers no matter how long the clip is.  `movietool probe` prints
// "duration vs RSS" to show that a 10 s clip costs no more than a 2 s one.

#include "movie.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <mach/mach.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace artemis
{
namespace movie
{

namespace
{

std::string NSErrorText(NSError* error)
{
	if(!error) return std::string();
	const char* utf8 = [[error localizedDescription] UTF8String];
	return utf8 ? std::string(utf8) : std::string("unknown AVFoundation error");
}

std::string FourCC(uint32_t code)
{
	char text[5];
	text[0] = (char)((code >> 24) & 0xff);
	text[1] = (char)((code >> 16) & 0xff);
	text[2] = (char)((code >> 8) & 0xff);
	text[3] = (char)(code & 0xff);
	text[4] = 0;
	for(int i = 0; i < 4; i++)
		if(text[i] < 0x20 || text[i] > 0x7e) text[i] = '?';
	return std::string(text);
}

int64_t MsFromCMTime(CMTime t)
{
	if(t.timescale == 0) return 0;
	const double seconds = (double)t.value / (double)t.timescale;
	return (int64_t)llround(seconds * 1000.0);
}

// How far before the requested moment a seek starts decoding.  Generous enough
// to cover a keyframe interval and a VFR gap, small enough to stay cheap.
constexpr int64_t kSeekBackMs = 400;

} // namespace

// ---------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------

void Canvas::Resize(int w, int h)
{
	width = w;
	height = h;
	rgba.assign((size_t)std::max(0, w) * (size_t)std::max(0, h) * 4, 0);
}

void Canvas::FillBlack()
{
	for(size_t i = 0; i + 3 < rgba.size(); i += 4)
	{
		rgba[i + 0] = 0;
		rgba[i + 1] = 0;
		rgba[i + 2] = 0;
		rgba[i + 3] = 255; // opaque: bg="black" is a colour, not transparency
	}
}

double Canvas::MeanLuma() const
{
	if(width <= 0 || height <= 0) return 0.0;
	const size_t pixels = (size_t)width * (size_t)height;
	double sum = 0.0;
	for(size_t i = 0; i < pixels; i++)
	{
		const uint8_t* p = &rgba[i * 4];
		sum += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
	}
	return sum / (double)pixels;
}

void Canvas::MeanRgb(double* r, double* g, double* b) const
{
	if(r) *r = 0.0;
	if(g) *g = 0.0;
	if(b) *b = 0.0;
	const size_t pixels = (size_t)width * (size_t)height;
	if(pixels == 0) return;
	double sr = 0.0, sg = 0.0, sb = 0.0;
	for(size_t i = 0; i < pixels; i++)
	{
		sr += rgba[i * 4 + 0];
		sg += rgba[i * 4 + 1];
		sb += rgba[i * 4 + 2];
	}
	if(r) *r = sr / (double)pixels;
	if(g) *g = sg / (double)pixels;
	if(b) *b = sb / (double)pixels;
}

// ---------------------------------------------------------------------------
// Movie::Impl
// ---------------------------------------------------------------------------

struct Movie::Impl
{
	AVURLAsset* asset = nil;
	AVAssetTrack* videoTrack = nil;
	AVAssetReader* reader = nil;
	AVAssetReaderTrackOutput* output = nil;
	std::string path;
	int64_t lastPtsMs = -1;
	bool atEnd = false;
};

std::unique_ptr<Movie> Movie::Open(const std::string& path, std::string* error)
{
	@autoreleasepool
	{
		NSString* nsPath = [NSString stringWithUTF8String:path.c_str()];
		if(!nsPath)
		{
			if(error) *error = "path is not valid UTF-8: " + path;
			return nullptr;
		}
		if(![[NSFileManager defaultManager] fileExistsAtPath:nsPath])
		{
			if(error) *error = "no such file: " + path;
			return nullptr;
		}

		NSURL* url = [NSURL fileURLWithPath:nsPath];
		AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url
		                                        options:@{AVURLAssetPreferPreciseDurationAndTimingKey : @YES}];
		if(!asset)
		{
			if(error) *error = "AVURLAsset could not open " + path;
			return nullptr;
		}

		NSArray<AVAssetTrack*>* videoTracks = [asset tracksWithMediaType:AVMediaTypeVideo];
		if(videoTracks.count == 0)
		{
			if(error) *error = "no video track in " + path;
			return nullptr;
		}
		AVAssetTrack* videoTrack = videoTracks[0];

		auto out = std::unique_ptr<Movie>(new Movie());
		out->impl_ = std::make_unique<Impl>();
		out->impl_->asset = asset;
		out->impl_->videoTrack = videoTrack;
		out->impl_->path = path;

		const CGSize size = [videoTrack naturalSize];
		out->info_.width = (int)llround(size.width);
		out->info_.height = (int)llround(size.height);

		const CMTime duration = [asset duration];
		out->info_.durationMs = MsFromCMTime(duration);

		const float nominalFps = [videoTrack nominalFrameRate];
		out->info_.fps = nominalFps > 0.01f ? (double)nominalFps : 0.0;
		if(out->info_.fps <= 0.0 && out->info_.durationMs > 0)
		{
			// Some VFR files report nothing useful; fall back to a nominal rate
			// only so the player has something to schedule with.  FrameAt() still
			// uses real timestamps, so a wrong guess here never picks a wrong frame.
			out->info_.fps = 30.0;
		}
		// An estimate.  `probe` counts the frames that actually come out.
		out->info_.frameCount = (int64_t)llround((double)out->info_.durationMs * out->info_.fps / 1000.0);

		NSArray* formatDescriptions = [videoTrack formatDescriptions];
		if(formatDescriptions.count > 0)
			out->info_.videoCodec = FourCC(CMFormatDescriptionGetMediaSubType(
			    (__bridge CMFormatDescriptionRef)formatDescriptions[0]));

		NSArray<AVAssetTrack*>* audioTracks = [asset tracksWithMediaType:AVMediaTypeAudio];
		out->info_.hasAudio = audioTracks.count > 0;
		if(out->info_.hasAudio)
		{
			AVAssetTrack* audioTrack = audioTracks[0];
			NSArray* audioFormats = [audioTrack formatDescriptions];
			if(audioFormats.count > 0)
			{
				CMAudioFormatDescriptionRef format =
				    (__bridge CMAudioFormatDescriptionRef)audioFormats[0];
				const AudioStreamBasicDescription* basic =
				    CMAudioFormatDescriptionGetStreamBasicDescription(format);
				if(basic)
				{
					out->info_.audioChannels = (int)basic->mChannelsPerFrame;
					out->info_.audioSampleRate = basic->mSampleRate;
				}
				out->info_.audioCodec =
				    FourCC(CMFormatDescriptionGetMediaSubType(format));
			}
		}

		if(out->info_.width <= 0 || out->info_.height <= 0)
		{
			if(error) *error = "video track reports no frame size";
			return nullptr;
		}
		return out;
	}
}

Movie::~Movie() = default;

namespace
{

// Starts (or restarts) the reader.  An AVAssetReader is one-shot: repositioning
// means throwing it away, which is why this is a separate step.
bool StartReader(AVAssetReader** readerOut, AVAssetReaderTrackOutput** outputOut, AVURLAsset* asset,
	AVAssetTrack* track, int64_t startMs, std::string* error)
{
	NSError* nsError = nil;
	AVAssetReader* reader = [[AVAssetReader alloc] initWithAsset:asset error:&nsError];
	if(!reader)
	{
		if(error) *error = "AVAssetReader: " + NSErrorText(nsError);
		return false;
	}

	// One frame in, one frame out, 32-bit BGRA: the cheapest thing CoreVideo can
	// hand us that the compositor can use after a channel swap.
	NSDictionary* settings = @{
		(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
	};
	AVAssetReaderTrackOutput* output =
	    [[AVAssetReaderTrackOutput alloc] initWithTrack:track outputSettings:settings];
	output.alwaysCopiesSampleData = NO;
	if(![reader canAddOutput:output])
	{
		if(error) *error = "reader refused the video track output";
		return false;
	}
	[reader addOutput:output];

	if(startMs > 0)
	{
		reader.timeRange = CMTimeRangeMake(CMTimeMake(startMs, 1000), kCMTimePositiveInfinity);
	}
	if(![reader startReading])
	{
		if(error) *error = "AVAssetReader failed to start: " + NSErrorText([reader error]);
		return false;
	}

	*readerOut = reader;
	*outputOut = output;
	return true;
}

} // namespace

bool Movie::Seek(int64_t ms, std::string* error)
{
	if(!impl_) return false;
	@autoreleasepool
	{
		if(ms < 0) ms = 0;
		const int64_t start = std::max<int64_t>(0, ms - kSeekBackMs);
		AVAssetReader* reader = nil;
		AVAssetReaderTrackOutput* output = nil;
		if(!StartReader(&reader, &output, impl_->asset, impl_->videoTrack, start, error)) return false;
		impl_->reader = reader;
		impl_->output = output;
		impl_->atEnd = false;
		impl_->lastPtsMs = start - 1;
		positionMs_ = start;
		frameIndex_ = 0;
		return true;
	}
}

int64_t Movie::FrameIndexAt(int64_t ms) const
{
	if(info_.fps <= 0.0) return 0;
	const int64_t index = (int64_t)std::floor((double)ms * info_.fps / 1000.0);
	if(info_.frameCount > 0) return std::min(std::max<int64_t>(0, index), info_.frameCount - 1);
	return std::max<int64_t>(0, index);
}

bool Movie::ReadNext(Canvas* out, std::string* error)
{
	if(!impl_ || !out) return false;
	if(impl_->atEnd) return false;
	@autoreleasepool
	{
		if(!impl_->reader)
		{
			AVAssetReader* reader = nil;
			AVAssetReaderTrackOutput* output = nil;
			if(!StartReader(&reader, &output, impl_->asset, impl_->videoTrack, 0, error)) return false;
			impl_->reader = reader;
			impl_->output = output;
		}

		CMSampleBufferRef sample = [impl_->output copyNextSampleBuffer];
		if(!sample)
		{
			impl_->atEnd = true;
			const AVAssetReaderStatus status = [impl_->reader status];
			if(status == AVAssetReaderStatusFailed)
			{
				if(error) *error = "decode failed: " + NSErrorText([impl_->reader error]);
			}
			return false; // end of file (or an error already described)
		}

		bool ok = false;
		CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(sample);
		if(pixels)
		{
			CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
			const size_t w = CVPixelBufferGetWidth(pixels);
			const size_t h = CVPixelBufferGetHeight(pixels);
			const size_t stride = CVPixelBufferGetBytesPerRow(pixels);
			const uint8_t* base = (const uint8_t*)CVPixelBufferGetBaseAddress(pixels);
			if(base && w > 0 && h > 0)
			{
				if(out->width != (int)w || out->height != (int)h) out->Resize((int)w, (int)h);
				for(size_t y = 0; y < h; y++)
				{
					const uint8_t* src = base + y * stride;
					uint8_t* dst = &out->rgba[y * w * 4];
					// BGRA -> RGBA, alpha forced opaque (video has no alpha here).
					for(size_t x = 0; x < w; x++)
					{
						dst[x * 4 + 0] = src[x * 4 + 2];
						dst[x * 4 + 1] = src[x * 4 + 1];
						dst[x * 4 + 2] = src[x * 4 + 0];
						dst[x * 4 + 3] = 255;
					}
				}
				ok = true;
			}
			CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
		}

		const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
		positionMs_ = MsFromCMTime(pts);
		impl_->lastPtsMs = positionMs_;
		if(ok) frameIndex_++;
		CFRelease(sample);
		return ok;
	}
}

bool Movie::FrameAt(int64_t ms, Canvas* out, std::string* error)
{
	if(!impl_) return false;
	if(ms < 0) ms = 0;

	// Start a little before the target and walk forward, keeping the last frame
	// whose real timestamp is <= ms.  This is timestamp-driven, so it is correct
	// for VFR where frame N is not at N/fps.
	if(!Seek(ms, error)) return false;

	Canvas scratch;
	Canvas previous;
	bool havePrevious = false;
	while(true)
	{
		std::string readError;
		if(!ReadNext(&scratch, &readError))
		{
			if(!readError.empty())
			{
				if(error) *error = readError;
				return false;
			}
			break; // hit the end of the file
		}
		if(positionMs_ > ms) break; // this frame is in the future
		previous = scratch;
		havePrevious = true;
	}

	if(!havePrevious)
	{
		// Everything decoded so far is after `ms` (a long first frame or a gap):
		// decode once from the start of the file and take that frame.
		if(!Seek(0, error)) return false;
		std::string readError;
		if(!ReadNext(&scratch, &readError))
		{
			if(error && !readError.empty()) *error = readError;
			return false;
		}
		*out = std::move(scratch);
		return true;
	}
	*out = std::move(previous);
	return true;
}

// ---------------------------------------------------------------------------
// MovieLayer
// ---------------------------------------------------------------------------

bool MovieLayer::Play(const std::string& path, bool clearToBlack, std::string* error)
{
	movie_ = Movie::Open(path, error);
	if(!movie_) return false;
	info_ = movie_->Info();
	clearToBlack_ = clearToBlack;
	if(clearToBlack_)
	{
		backdrop_.Resize(info_.width, info_.height);
		backdrop_.FillBlack();
	}
	else
	{
		backdrop_ = Canvas{};
	}
	finished_ = false;
	currentFrame_ = -1;
	framesShown_ = 0;
	seekCount_ = 0;
	seekTotalMs_ = 0.0;
	return true;
}

void MovieLayer::Cancel()
{
	movie_.reset();
	finished_ = true;
}

bool MovieLayer::Advance(int64_t elapsedMs, Canvas* out)
{
	if(!movie_ || !out) return false;
	if(finished_) return false;

	if(elapsedMs >= info_.durationMs && info_.durationMs > 0)
	{
		finished_ = true;
		return false;
	}

	const int64_t wanted = movie_->FrameIndexAt(elapsedMs);
	if(wanted != currentFrame_)
	{
		const double started = NowSeconds();
		std::string error;
		if(!movie_->FrameAt(elapsedMs, out, &error))
		{
			finished_ = true;
			return false;
		}
		seekTotalMs_ += (NowSeconds() - started) * 1000.0;
		seekCount_++;
		currentFrame_ = wanted;
		framesShown_++;
		return true;
	}

	// Same frame as last time: hold it rather than decoding again.
	if(out->Empty())
	{
		// First call with no decode yet, and bg="black" was asked for.
		out->Resize(info_.width, info_.height);
		out->FillBlack();
	}
	return true;
}

// ---------------------------------------------------------------------------
// Probe
// ---------------------------------------------------------------------------

bool Probe(const std::string& path, ProbeResult* result, std::string* error)
{
	auto movie = Movie::Open(path, error);
	if(!movie) return false;

	ProbeResult local;
	const int64_t rssBefore = ResidentBytes();
	Canvas canvas;
	const double started = NowSeconds();
	int64_t frame = 0;
	double sumLuma = 0.0, sumSquares = 0.0;
	double minLuma = 1e9, maxLuma = -1e9;

	while(true)
	{
		std::string readError;
		if(!movie->ReadNext(&canvas, &readError))
		{
			if(!readError.empty())
			{
				if(error) *error = readError;
				return false;
			}
			break;
		}
		const double luma = canvas.MeanLuma();
		if(frame == 0)
		{
			local.firstFrameLuma = (int64_t)llround(luma);
			canvas.MeanRgb(&local.firstR, &local.firstG, &local.firstB);
			canvas = Canvas{}; // drop immediately: one frame at a time
		}
		local.lastFrameLuma = (int64_t)llround(luma);
		canvas.MeanRgb(&local.lastR, &local.lastG, &local.lastB);
		minLuma = std::min(minLuma, luma);
		maxLuma = std::max(maxLuma, luma);
		sumLuma += luma;
		sumSquares += luma * luma;
		frame++;
		canvas = Canvas{};
	}

	local.framesDecoded = frame;
	local.secondsWall = NowSeconds() - started;
	local.minLuma = frame ? minLuma : 0.0;
	local.maxLuma = frame ? maxLuma : 0.0;
	if(frame > 1)
	{
		const double mean = sumLuma / (double)frame;
		const double variance = sumSquares / (double)frame - mean * mean;
		local.lumaVariance = variance > 0 ? std::sqrt(variance) : 0.0;
	}
	local.peakRssDeltaBytes = ResidentBytes() - rssBefore;
	if(result) *result = local;
	return true;
}

double NowSeconds()
{
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

int64_t ResidentBytes()
{
	mach_task_basic_info_data_t info;
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
	if(task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return 0;
	return (int64_t)info.resident_size;
}

} // namespace movie
} // namespace artemis
