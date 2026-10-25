// device.h - real CoreAudio output for the Artemis mixer
//
// Why CoreAudio's AudioUnit and not SDL2:
//   * `audiotool` links SDL2 but `artemis_audio` does not, and the mixer is the
//     thing that has to reach a speaker in `agame` too.  AudioUnit needs only
//     AudioToolbox + CoreAudio, which artemis_audio already links, so the device
//     path costs the project no new dependency and no CMake change.
//   * AudioUnit hands the callback the device's own render buffer, so
//     `Engine::Mix` writes straight into it - no copy, no extra thread.
//   * The default output device on this machine runs at **48000 Hz** while the
//     shipped audio is 44100.  That is genuinely useful: the mixer's resampler
//     is exercised by every real playback instead of only in tests.
//
// The callback obeys the usual realtime rules: no locks, no allocation, no file
// I/O, no logging.  Measurement data is written into fixed-size arrays that were
// allocated when the device was opened, and percentiles are computed later from
// a non-realtime thread.
//
// Nothing here is required for offline use: if the machine has no output device
// (a headless session, a locked-down CI box) QueryDefault() says so with a
// reason and the caller stays in offline mode.  Nothing fails silently.

#pragma once

#include "audio.h"

#include <cstdint>
#include <string>
#include <vector>

namespace artemis {
namespace audio {

struct AudioDevice {
	uint32_t id = 0;
	std::string name;
	int outputChannels = 0;
	double sampleRate = 0.0;
	bool isDefault = false;
};

struct DeviceList {
	std::vector<AudioDevice> devices;
	int defaultIndex = -1; // index into devices, -1 if there is no default out
};

// What Open() will talk to.  Filled in by QueryDefault() so the caller can open
// its Engine at the device's rate before the device exists.
struct DeviceTarget {
	bool available = false;
	uint32_t id = 0;
	std::string name;
	double sampleRate = 0.0;
	int bufferFrames = 0;
	int channels = 2;
	std::string reason; // why it is unavailable, when available == false
};

// Timing recorded inside the audio callback.
struct CallbackStats {
	uint64_t callbacks = 0;
	uint64_t frames = 0;
	uint64_t underruns = 0; // callbacks that began with an empty decoder buffer
	uint64_t dropped = 0;   // callbacks that could not be recorded (array full)

	double intervalP50Ms = 0.0;
	double intervalP99Ms = 0.0;
	double intervalMaxMs = 0.0;
	double intervalJitterMs = 0.0; // standard deviation of the interval

	double workP50Us = 0.0; // time spent inside Engine::Mix
	double workP99Us = 0.0;
	double workMaxUs = 0.0;
};

class Device {
public:
	enum class Mode {
		Closed,
		CoreAudio, // real hardware
		Offline,   // no hardware; the caller renders on its own clock
		Failed,
	};

	static DeviceList Enumerate();

	// Asks the system for its default output device and the buffer size it
	// wants.  Never opens the device, so it is safe to call before the Engine
	// exists (the Engine has to be created at the device's sample rate).
	static DeviceTarget QueryDefault();

	~Device();

	// Attaches to `engine`, which must have been opened with the target's sample
	// rate.  On failure it returns false, sets `reason`, and leaves the object
	// in Mode::Failed - the caller is expected to fall back to offline and say
	// why.
	bool Open(Engine* engine, const DeviceTarget& target, std::string* reason);
	void Close();

	bool Start(std::string* reason);
	void Stop();
	bool IsRunning() const { return running_; }

	Mode mode() const { return mode_; }
	const std::string& deviceName() const { return deviceName_; }
	const std::string& fallbackReason() const { return fallbackReason_; }
	double SampleRate() const { return sampleRate_; }
	int BufferFrames() const { return bufferFrames_; }
	const CallbackStats& Stats() const { return stats_; }

	// The body the audio callback runs.  Public only so the C callback in
	// device.cpp can reach it; it obeys the realtime rules itself and must be
	// called with the device's render buffer.
	void RenderInCallback(uint32_t frames, void* bufferList);

	// Recomputes percentiles from what the callback recorded.  Call from a
	// normal (non-realtime) thread.
	void RefreshStats();

	// Runs `frames` of mixing on the calling thread through the exact function
	// the audio callback uses, and reports how many heap blocks were allocated
	// while it ran.  This is the evidence that the mixer does not allocate: the
	// real callback cannot call this (it would itself allocate), so the audit
	// reuses the callback body on a normal thread.
	static int64_t AuditMixerAllocations(Engine* engine, int frames, int iterations, int* worstIteration);

private:
	Engine* engine_ = nullptr;
	Mode mode_ = Mode::Closed;
	bool running_ = false;
	std::string deviceName_;
	std::string fallbackReason_;
	double sampleRate_ = 0.0;
	int bufferFrames_ = 0;
	int channels_ = 2;
	CallbackStats stats_;

	void* unit_ = nullptr; // AudioUnit

	// Fixed-size capture, allocated when the device opens.
	std::vector<double> intervalMs_;
	std::vector<double> workUs_;
	uint64_t lastTick_ = 0;
};

} // namespace audio
} // namespace artemis
