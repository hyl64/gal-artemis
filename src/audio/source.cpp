// source.cpp - Ogg Vorbis decoding, ring buffer and the per-voice mixer stream
//
// Decoding goes through CoreAudio's ExtAudioFile (AudioToolbox), a plain C API
// that on macOS reads Ogg Vorbis natively.  Three reasons this is the right
// layer rather than bundling libvorbis:
//
//   * it is the same parser `afinfo` reports through, so every number the
//     tool prints can be checked against a system tool;
//   * it is a C API, so no Objective-C++ is needed and the file stays .cpp
//     (the project's CMake globs src/audio/*.cpp);
//   * it streams.  ExtAudioFile pulls packets on demand, so decoding a 3.4 MB
//     BGM track does not map 3.4 MB of memory - measured by `audiotool probe`.
//
// What CoreAudio will *not* do for us is sample-rate conversion through the
// client format, and the shipped data needs it: BGM is 44100 Hz but
// se/kisya.ogg is 48000 Hz.  Stream therefore converts to the engine rate with
// linear interpolation, and upmixes mono to stereo.

#include "audio.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sys/stat.h>

namespace artemis {
namespace audio {

namespace {

// Four-character code of an AudioFormatID, for reporting.  CoreAudio hands
// back 'vorb' for an Ogg Vorbis file, which is a useful thing to assert on.
std::string FourCC(UInt32 code) {
	char s[5];
	s[0] = (char)((code >> 24) & 0xff);
	s[1] = (char)((code >> 16) & 0xff);
	s[2] = (char)((code >> 8) & 0xff);
	s[3] = (char)(code & 0xff);
	s[4] = 0;
	for (int i = 0; i < 4; i++)
		if (s[i] < 0x20 || s[i] > 0x7e) s[i] = '?';
	return std::string(s);
}

// Single-producer/single-consumer ring of interleaved stereo frames.  The
// decoder thread writes, the mixer thread reads; the two counters make that
// safe without a mutex, which is what keeps Stream::Mix() callable from an
// audio callback.
class FrameRing {
public:
	void Resize(int frames) {
		capacity_ = frames > 0 ? frames : 1;
		buffer_.assign((size_t)capacity_ * 2, 0.0f);
		write_.store(0, std::memory_order_relaxed);
		read_.store(0, std::memory_order_relaxed);
	}

	int Capacity() const { return capacity_; }

	int Readable() const {
		return (int)(write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire));
	}
	int Writable() const { return capacity_ - Readable(); }

	int Write(const float* src, int frames) {
		if (frames <= 0) return 0;
		int space = Writable();
		if (frames > space) frames = space;
		if (frames <= 0) return 0;
		const uint64_t w = write_.load(std::memory_order_relaxed);
		const int start = (int)(w % (uint64_t)capacity_);
		const int first = std::min(frames, capacity_ - start);
		std::memcpy(&buffer_[(size_t)start * 2], src, (size_t)first * 2 * sizeof(float));
		if (first < frames)
			std::memcpy(&buffer_[0], src + (size_t)first * 2, (size_t)(frames - first) * 2 * sizeof(float));
		write_.store(w + (uint64_t)frames, std::memory_order_release);
		return frames;
	}

	int Read(float* dst, int frames) {
		if (frames <= 0) return 0;
		int have = Readable();
		if (frames > have) frames = have;
		if (frames <= 0) return 0;
		const uint64_t r = read_.load(std::memory_order_relaxed);
		const int start = (int)(r % (uint64_t)capacity_);
		const int first = std::min(frames, capacity_ - start);
		std::memcpy(dst, &buffer_[(size_t)start * 2], (size_t)first * 2 * sizeof(float));
		if (first < frames)
			std::memcpy(dst + (size_t)first * 2, &buffer_[0], (size_t)(frames - first) * 2 * sizeof(float));
		read_.store(r + (uint64_t)frames, std::memory_order_release);
		return frames;
	}

	void Clear() {
		read_.store(0, std::memory_order_relaxed);
		write_.store(0, std::memory_order_relaxed);
	}

private:
	std::vector<float> buffer_;
	int capacity_ = 0;
	std::atomic<uint64_t> write_{0};
	std::atomic<uint64_t> read_{0};
};

// Source frames decoded per refill.  Large enough that the decoder is not
// asked for tiny reads, small enough to stay in cache.
constexpr int kLookaheadFrames = 8192;
// Mixer processes at most this many frames per internal chunk, so a caller
// with a big tick cannot overflow the per-stream scratch.
constexpr int kMixChunk = 2048;
// Longest loop body that may be recorded for gapless A-B repeat.  45 s of
// stereo float32 is about 16 MB, which is a sane ceiling for a BGM loop;
// anything longer falls back to seeking at the seam.
constexpr int kMaxLoopCacheSeconds = 45;

} // namespace

// ---------------------------------------------------------------------------
// Source
// ---------------------------------------------------------------------------

struct Source::Impl {
	ExtAudioFileRef file = nullptr;
	CFURLRef url = nullptr;
	~Impl() {
		if (file) ExtAudioFileDispose(file);
		if (url) CFRelease(url);
	}
};

std::unique_ptr<Source> Source::Open(const std::string& path, std::string* error) {
	auto fail = [&](const std::string& why) -> std::unique_ptr<Source> {
		if (error) *error = why;
		return nullptr;
	};

	struct stat st;
	int64_t fileBytes = 0;
	if (::stat(path.c_str(), &st) == 0) fileBytes = (int64_t)st.st_size;

	auto impl = std::make_unique<Impl>();
	impl->url = CFURLCreateFromFileSystemRepresentation(
	    kCFAllocatorDefault, (const UInt8*)path.c_str(), (CFIndex)path.size(), false);
	if (!impl->url) return fail("cannot build URL for " + path);

	OSStatus status = ExtAudioFileOpenURL(impl->url, &impl->file);
	if (status != noErr || !impl->file)
		return fail("ExtAudioFileOpenURL failed (" + std::to_string((int)status) + ") for " + path);

	AudioStreamBasicDescription fileFormat{};
	UInt32 size = sizeof(fileFormat);
	status = ExtAudioFileGetProperty(impl->file, kExtAudioFileProperty_FileDataFormat, &size, &fileFormat);
	if (status != noErr) return fail("cannot read file data format");

	SInt64 lengthFrames = 0;
	size = sizeof(lengthFrames);
	status = ExtAudioFileGetProperty(impl->file, kExtAudioFileProperty_FileLengthFrames, &size, &lengthFrames);
	if (status != noErr) return fail("cannot read file length in frames");

	// Ask for float32, packed, interleaved, at the file's own rate and channel
	// count.  Leaving the rate alone keeps CoreAudio out of resampling; the
	// mixer does that itself so the behaviour is visible and testable.
	AudioStreamBasicDescription client{};
	client.mSampleRate = fileFormat.mSampleRate;
	client.mFormatID = kAudioFormatLinearPCM;
	client.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
	client.mChannelsPerFrame = fileFormat.mChannelsPerFrame;
	client.mBitsPerChannel = 32;
	client.mFramesPerPacket = 1;
	client.mBytesPerFrame = 4 * client.mChannelsPerFrame;
	client.mBytesPerPacket = client.mBytesPerFrame;
	status = ExtAudioFileSetProperty(impl->file, kExtAudioFileProperty_ClientDataFormat, sizeof(client), &client);
	if (status != noErr)
		return fail("cannot set float32 client format (" + std::to_string((int)status) + ")");

	auto out = std::unique_ptr<Source>(new Source());
	out->impl_ = std::move(impl);
	out->info_.sampleRate = (int)std::lround(fileFormat.mSampleRate);
	out->info_.channels = (int)fileFormat.mChannelsPerFrame;
	out->info_.frames = (int64_t)lengthFrames;
	out->info_.fileBytes = fileBytes;
	out->info_.codec = FourCC(fileFormat.mFormatID);
	out->position_ = 0;
	return out;
}

Source::~Source() = default;

int64_t Source::Read(float* out, int64_t frames) {
	if (!impl_ || !impl_->file || frames <= 0 || info_.channels <= 0) return 0;
	const int ch = info_.channels;
	int64_t got = 0;
	while (got < frames) {
		UInt32 want = (UInt32)std::min<int64_t>(frames - got, 1 << 18);
		AudioBufferList list;
		list.mNumberBuffers = 1;
		list.mBuffers[0].mNumberChannels = (UInt32)ch;
		list.mBuffers[0].mData = out + got * ch;
		list.mBuffers[0].mDataByteSize = (UInt32)(want * (UInt32)ch * sizeof(float));
		OSStatus status = ExtAudioFileRead(impl_->file, &want, &list);
		if (status != noErr) break; // decoder complained; report what we have
		if (want == 0) break;       // end of file
		got += want;
	}
	position_ += got;
	return got;
}

bool Source::Seek(int64_t frame) {
	if (!impl_ || !impl_->file) return false;
	if (frame < 0) frame = 0;
	if (frame > info_.frames) frame = info_.frames;
	if (ExtAudioFileSeek(impl_->file, (SInt64)frame) != noErr) return false;
	position_ = frame;
	return true;
}

// ---------------------------------------------------------------------------
// Stream
// ---------------------------------------------------------------------------

struct Stream::Impl {
	std::unique_ptr<Source> source;
	FrameRing ring;

	std::vector<float> sourceBuffer; // native interleaved, kLookaheadFrames
	int64_t sourceBase = 0;          // index of sourceBuffer[0]
	int64_t sourceCount = 0;         // frames valid in sourceBuffer

	double position = 0.0;           // fractional position, source frames
	int channels = 0;
	int sourceRate = 0;
	int engineRate = 44100;

	std::vector<float> convert; // engine-domain interleaved stereo scratch
	std::vector<float> mixChunk;
	std::vector<float> readChunk;

	// Mixer-thread-only state.
	float gain = 1.0f;
	float gainStep = 0.0f;
	int fadeFramesLeft = 0;
	uint32_t seenFadeGen = 0;

	int64_t framesDecoded = 0;

	// Recorded loop body.  Seeking back to a loop point is packet-aligned and
	// would click at every seam, so the first pass through [loopStart,loopEnd)
	// is captured as it goes and every later repeat is played from this buffer.
	// Only enabled when no resampling is needed, which is the case for BGM.
	std::vector<float> loopCache;
	int64_t loopCacheFrames = 0;   // frames the loop body is expected to hold
	bool loopCacheEnabled = false;
	bool loopCacheComplete = false;
	bool loopCachePlaying = false;
	int64_t loopCachePos = 0;

	// Serialises Start() (control thread) against Fill() (decoder thread).
	// The mixer never takes it, so Mix() stays lock-free.
	std::mutex guard;

	const float* FramePtr(int64_t index) const {
		if (index < sourceBase || index >= sourceBase + sourceCount) return nullptr;
		return &sourceBuffer[(size_t)(index - sourceBase) * (size_t)channels];
	}

	// Makes sure the pair (index, index+1) is in memory.
	bool Ensure(int64_t index);
};

bool Stream::Impl::Ensure(int64_t index) {
	if (index >= sourceBase && index + 1 < sourceBase + sourceCount) return true;

	// Sequential case: one frame short of the window.  Carry that frame over and
	// append, because seeking a compressed stream is packet-aligned (see below)
	// and would shift every following sample.  This is the normal path while
	// playing through a track.
	if (sourceCount > 0 && index == sourceBase + sourceCount - 1) {
		const int ch = channels;
		float carried[8];
		if (ch > 8) return false;
		std::memcpy(carried, &sourceBuffer[(size_t)(index - sourceBase) * (size_t)ch],
		            sizeof(float) * (size_t)ch);
		sourceBase = index;
		sourceCount = 1 + source->Read(sourceBuffer.data() + ch, kLookaheadFrames - 1);
		std::memcpy(sourceBuffer.data(), carried, sizeof(float) * (size_t)ch);
		return sourceCount > 1;
	}

	// Random access.  Measured on real BGM: ExtAudioFileSeek(441000) returns
	// audio that starts at source frame 441896 - the decoder lands on a packet
	// boundary, so a seek is *not* sample-accurate even though the samples it
	// returns are exact.  The A-B loop therefore never seeks back; Fill()
	// records the loop body during the first pass instead.
	if (!source->Seek(index)) return false;
	sourceBase = index;
	sourceCount = source->Read(sourceBuffer.data(), kLookaheadFrames);
	return sourceCount > 0;
}

std::shared_ptr<Stream> Stream::Open(const std::string& path, const Config& cfg, std::string* error) {
	auto source = Source::Open(path, error);
	if (!source) return nullptr;

	std::shared_ptr<Stream> stream(new Stream());
	stream->impl_ = std::make_unique<Impl>();
	Impl& impl = *stream->impl_;
	impl.source = std::move(source);
	impl.channels = impl.source->Info().channels;
	impl.sourceRate = impl.source->Info().sampleRate;
	impl.engineRate = cfg.engineRate > 0 ? cfg.engineRate : 44100;
	impl.ring.Resize(cfg.ringFrames > 0 ? cfg.ringFrames : 16384);
	impl.sourceBuffer.assign((size_t)kLookaheadFrames * (size_t)std::max(1, impl.channels), 0.0f);
	impl.convert.assign((size_t)cfg.ringFrames * 2 + 8, 0.0f);
	impl.mixChunk.assign((size_t)kMixChunk * 2, 0.0f);
	impl.readChunk.assign((size_t)kMixChunk * 2, 0.0f);
	impl.gain = cfg.volume;

	stream->info_ = impl.source->Info();
	stream->plan_ = LoopPlan::Whole(stream->info_.frames);
	stream->fadeTarget_.store(cfg.volume);
	return stream;
}

void Stream::Start(const LoopPlan& plan) {
	std::lock_guard<std::mutex> lock(impl_->guard);
	plan_ = plan;
	if (plan_.frames <= 0) plan_.frames = info_.frames;
	impl_->position = 0.0;
	impl_->sourceBase = 0;
	impl_->sourceCount = 0;
	impl_->ring.Clear();
	impl_->framesDecoded = 0;
	impl_->gain = fadeTarget_.load(std::memory_order_relaxed);
	impl_->gainStep = 0.0f;
	impl_->fadeFramesLeft = 0;
	impl_->seenFadeGen = fadeGen_.load(std::memory_order_acquire);
	finished_.store(false, std::memory_order_release);
	stopped_.store(false, std::memory_order_release);
	// Rewind the decoder so the intro really plays from the first frame.
	impl_->source->Seek(0);

	// Loop body recording: only possible without resampling, and only for loop
	// bodies small enough to hold.  A-B repeat then needs no seek at all, which
	// matters because a seek lands on a packet boundary (measured: a request for
	// frame 441000 returns frame 441896) and would click at every seam.
	const int64_t loopFrames = plan_.looping ? plan_.loopEnd - plan_.loopStart : 0;
	impl_->loopCacheEnabled = loopFrames > 0 && impl_->sourceRate == impl_->engineRate &&
	                          loopFrames <= (int64_t)impl_->engineRate * kMaxLoopCacheSeconds;
	impl_->loopCacheFrames = loopFrames;
	impl_->loopCacheComplete = false;
	impl_->loopCachePlaying = false;
	impl_->loopCachePos = 0;
	impl_->loopCache.clear();
	if (impl_->loopCacheEnabled)
		impl_->loopCache.reserve((size_t)loopFrames * 2);
}

int Stream::Fill() {
	if (!impl_) return 0;
	std::lock_guard<std::mutex> lock(impl_->guard);
	if (finished_.load(std::memory_order_acquire) || stopped_.load(std::memory_order_acquire)) return 0;

	Impl& impl = *impl_;
	int space = impl.ring.Writable();
	if (space <= 0) return 0;

	int budget = std::min(space, 4096);
	int produced = 0;
	const double step = (double)impl.sourceRate / (double)impl.engineRate;
	const bool looping = plan_.looping && plan_.loopEnd > plan_.loopStart;
	const double loopLen = looping ? (double)(plan_.loopEnd - plan_.loopStart) : 0.0;

	while (produced < budget) {
		// Repeating the recorded loop body: no decoder, no seek, and the seam is
		// sample exact because these are the very samples that played the first
		// time round.
		if (impl.loopCachePlaying) {
			if (impl.loopCachePos >= impl.loopCacheFrames) impl.loopCachePos = 0;
			const size_t at = (size_t)impl.loopCachePos * 2;
			impl.convert[(size_t)produced * 2 + 0] = impl.loopCache[at + 0];
			impl.convert[(size_t)produced * 2 + 1] = impl.loopCache[at + 1];
			impl.loopCachePos++;
			produced++;
			continue;
		}

		if (looping) {
			if (impl.position >= (double)plan_.loopEnd) {
				if (impl.loopCacheComplete && impl.loopCacheFrames > 0) {
					impl.loopCachePlaying = true;
					impl.loopCachePos = 0;
					continue;
				}
				// Fallback for a loop body too large to record: jump back in the
				// fractional domain.  Correct in content, but the decoder has to
				// seek, so this seam is packet aligned rather than sample exact.
				double over = impl.position - (double)plan_.loopEnd;
				impl.position = (double)plan_.loopStart + std::fmod(over, loopLen);
			}
		} else if (impl.position >= (double)info_.frames) {
			finished_.store(true, std::memory_order_release);
			break;
		}

		const int64_t i0 = (int64_t)std::floor(impl.position);
		const double frac = impl.position - (double)i0;
		if (!impl.Ensure(i0)) {
			finished_.store(true, std::memory_order_release);
			break;
		}

		const float* a = impl.FramePtr(i0);
		const float* b = impl.FramePtr(i0 + 1);
		float left, right;
		if (impl.channels == 1) {
			const float s = (float)(a[0] * (1.0 - frac) + (b ? b[0] : 0.0f) * frac);
			left = right = s;
		} else {
			left = (float)(a[0] * (1.0 - frac) + (b ? b[0] : 0.0f) * frac);
			right = (float)(a[1] * (1.0 - frac) + (b ? b[1] : 0.0f) * frac);
			if (impl.channels > 2) {
				// More than stereo is not used by these titles; fold the rest
				// in so nothing is silently dropped.
				for (int c = 2; c < impl.channels; c++)
					left += (float)(a[c] * (1.0 - frac) + (b ? b[c] : 0.0f) * frac) * 0.5f;
			}
		}

		impl.convert[(size_t)produced * 2 + 0] = std::max(-1.0f, std::min(1.0f, left));
		impl.convert[(size_t)produced * 2 + 1] = std::max(-1.0f, std::min(1.0f, right));

		// Capture the loop body as it sounds on the first pass.
		if (impl.loopCacheEnabled && !impl.loopCacheComplete && impl.position >= (double)plan_.loopStart &&
		    impl.position < (double)plan_.loopEnd) {
			impl.loopCache.push_back(impl.convert[(size_t)produced * 2 + 0]);
			impl.loopCache.push_back(impl.convert[(size_t)produced * 2 + 1]);
			if ((int64_t)(impl.loopCache.size() / 2) >= impl.loopCacheFrames)
				impl.loopCacheComplete = true;
		}

		produced++;
		impl.position += step;
	}

	if (produced > 0) {
		impl.ring.Write(impl.convert.data(), produced);
		impl.framesDecoded += produced;
	}
	return produced;
}

void Stream::Mix(float* out, int frames) {
	if (!impl_ || !out || frames <= 0) return;
	if (stopped_.load(std::memory_order_acquire)) return;
	Impl& impl = *impl_;

	// Pick up a fade requested since the last tick.
	const uint32_t gen = fadeGen_.load(std::memory_order_acquire);
	if (gen != impl.seenFadeGen) {
		impl.seenFadeGen = gen;
		const float target = fadeTarget_.load(std::memory_order_relaxed);
		const int ms = fadeMs_.load(std::memory_order_relaxed);
		const float from = fadeFrom_.load(std::memory_order_relaxed);
		if (from >= 0.0f) impl.gain = from; // explicit fade-in start
		const int fadeFrames = ms > 0 ? (int)((int64_t)ms * impl.engineRate / 1000) : 0;
		if (fadeFrames <= 0) {
			impl.gain = target;
			impl.gainStep = 0.0f;
			impl.fadeFramesLeft = 0;
		} else {
			impl.fadeFramesLeft = fadeFrames;
			impl.gainStep = (target - impl.gain) / (float)fadeFrames;
		}
	}

	int done = 0;
	while (done < frames) {
		const int chunk = std::min(frames - done, kMixChunk);
		const int got = impl.ring.Read(impl.readChunk.data(), chunk);
		for (int i = 0; i < got; i++) {
			if (impl.fadeFramesLeft > 0) {
				impl.gain += impl.gainStep;
				if (--impl.fadeFramesLeft == 0) {
					impl.gain = fadeTarget_.load(std::memory_order_relaxed);
					// A completed fade to silence ends the stream, which is how
					// a crossfaded-out BGM leaves its slot.
					if (impl.gain <= 0.0001f) stopped_.store(true, std::memory_order_release);
				}
			}
			const float g = impl.gain;
			out[(size_t)(done + i) * 2 + 0] += impl.readChunk[(size_t)i * 2 + 0] * g;
			out[(size_t)(done + i) * 2 + 1] += impl.readChunk[(size_t)i * 2 + 1] * g;
		}
		done += chunk;
		if (got < chunk) break; // ring ran dry; nothing more to add this tick
	}
}

int Stream::Buffered() { return impl_ ? impl_->ring.Readable() : 0; }

void Stream::FadeTo(float target, int ms) {
	fadeFrom_.store(-1.0f, std::memory_order_relaxed);
	fadeTarget_.store(target, std::memory_order_relaxed);
	fadeMs_.store(ms, std::memory_order_relaxed);
	fadeGen_.fetch_add(1, std::memory_order_release);
}

void Stream::FadeFromTo(float from, float to, int ms) {
	fadeFrom_.store(from, std::memory_order_relaxed);
	fadeTarget_.store(to, std::memory_order_relaxed);
	fadeMs_.store(ms, std::memory_order_relaxed);
	fadeGen_.fetch_add(1, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

int64_t ResidentBytes() {
	mach_task_basic_info_data_t info;
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return 0;
	return (int64_t)info.resident_size;
}

double NowSeconds() {
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

} // namespace audio
} // namespace artemis
