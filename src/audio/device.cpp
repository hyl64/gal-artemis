// device.cpp - CoreAudio default-output transport for the mixer
//
// The whole point of this file is that RenderCallback below obeys the realtime
// contract while still producing numbers we can assert on:
//
//   * no locks        - the callback touches only atomics-owned state and
//                       fixed arrays; Engine::Mix takes no mutex (proven by
//                       AuditMixerAllocations and by construction of the ring)
//   * no allocation   - every buffer is allocated in Open(), and the capture
//                       arrays are pre-sized there too
//   * no I/O, no printf, no exceptions
//
// Timing is captured with mach_absolute_time into preallocated vectors and the
// percentiles are computed later, off the audio thread.
//
// Measured on this machine: the default output device is the built-in speakers
// at 48000 Hz, while shipped game audio is 44100 Hz, so the mixer's resampler
// runs on every real playback rather than only in the self-test.

#include "device.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <malloc/malloc.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace artemis {
namespace audio {

namespace {

constexpr size_t kMaxSamples = 8192; // ~2 minutes of callbacks at 512 frames

std::string CFToString(CFStringRef s) {
	if (!s) return std::string();
	char buf[256];
	if (!CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) return std::string();
	return std::string(buf);
}

std::string DeviceName(AudioDeviceID id) {
	AudioObjectPropertyAddress address{kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
	                                   kAudioObjectPropertyElementMain};
	CFStringRef name = nullptr;
	UInt32 size = sizeof(name);
	if (AudioObjectGetPropertyData(id, &address, 0, nullptr, &size, &name) != noErr || !name) return std::string();
	std::string out = CFToString(name);
	CFRelease(name);
	return out;
}

int OutputChannels(AudioDeviceID id) {
	AudioObjectPropertyAddress address{kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput,
	                                   kAudioObjectPropertyElementMain};
	UInt32 size = 0;
	if (AudioObjectGetPropertyDataSize(id, &address, 0, nullptr, &size) != noErr || size == 0) return 0;
	std::vector<uint8_t> storage(size);
	auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
	if (AudioObjectGetPropertyData(id, &address, 0, nullptr, &size, list) != noErr) return 0;
	int channels = 0;
	for (UInt32 i = 0; i < list->mNumberBuffers; i++) channels += (int)list->mBuffers[i].mNumberChannels;
	return channels;
}

double DeviceSampleRate(AudioDeviceID id) {
	AudioObjectPropertyAddress address{kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
	                                   kAudioObjectPropertyElementMain};
	Float64 rate = 0;
	UInt32 size = sizeof(rate);
	if (AudioObjectGetPropertyData(id, &address, 0, nullptr, &size, &rate) != noErr) return 0.0;
	return rate;
}

UInt32 DeviceBufferFrames(AudioDeviceID id) {
	// The HAL reports the device's preferred page size; the AudioUnit will honour
	// it unless someone overrides it.  Not every device exposes it.
	AudioObjectPropertyAddress address{kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
	                                   kAudioObjectPropertyElementMain};
	UInt32 frames = 0;
	UInt32 size = sizeof(frames);
	if (AudioObjectGetPropertyData(id, &address, 0, nullptr, &size, &frames) != noErr) return 0;
	return frames;
}

AudioDeviceID DefaultOutputDevice() {
	AudioObjectPropertyAddress address{kAudioHardwarePropertyDefaultOutputDevice,
	                                   kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
	AudioDeviceID id = kAudioObjectSystemObject;
	UInt32 size = sizeof(id);
	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &id) != noErr)
		return kAudioObjectSystemObject;
	return id;
}

double TicksToSeconds(uint64_t ticks, const mach_timebase_info_data_t& base) {
	return (double)ticks * (double)base.numer / (double)base.denom / 1e9;
}

double Percentile(std::vector<double> values, double fraction) {
	if (values.empty()) return 0.0;
	std::sort(values.begin(), values.end());
	const size_t index = (size_t)(fraction * (double)(values.size() - 1));
	return values[std::min(index, values.size() - 1)];
}

} // namespace

Device::~Device() { Close(); }

DeviceList Device::Enumerate() {
	DeviceList list;
	AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
	                                   kAudioObjectPropertyElementMain};
	UInt32 size = 0;
	if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr) return list;

	const UInt32 count = size / sizeof(AudioDeviceID);
	std::vector<AudioDeviceID> ids(count);
	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, ids.data()) != noErr)
		return list;

	const AudioDeviceID def = DefaultOutputDevice();
	for (AudioDeviceID id : ids) {
		AudioDevice device;
		device.id = id;
		device.name = DeviceName(id);
		device.outputChannels = OutputChannels(id);
		device.sampleRate = DeviceSampleRate(id);
		device.isDefault = (id == def);
		// Only devices that can actually play are interesting here.
		if (device.outputChannels <= 0) continue;
		if (device.isDefault) list.defaultIndex = (int)list.devices.size();
		list.devices.push_back(std::move(device));
	}
	return list;
}

DeviceTarget Device::QueryDefault() {
	DeviceTarget target;
	const AudioDeviceID id = DefaultOutputDevice();
	if (id == kAudioObjectSystemObject) {
		target.reason = "no default output device reported by the HAL";
		return target;
	}
	const int channels = OutputChannels(id);
	if (channels <= 0) {
		target.reason = "default device has no output channels (a capture-only device?)";
		return target;
	}
	target.id = id;
	target.name = DeviceName(id);
	if (target.name.empty()) target.name = "default output";
	target.channels = channels >= 2 ? 2 : 1;
	target.sampleRate = DeviceSampleRate(id);
	if (target.sampleRate <= 0) {
		// A device that will not state its rate still works: AudioUnit defaults to
		// 44100, and the mixer can run at that.
		target.sampleRate = 44100.0;
	}
	target.bufferFrames = (int)DeviceBufferFrames(id);
	target.available = true;
	return target;
}

// ---------------------------------------------------------------------------
// the render callback
// ---------------------------------------------------------------------------
namespace {

OSStatus RenderCallback(void* refCon, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* stamp, UInt32 bus,
                        UInt32 frames, AudioBufferList* data) {
	auto* device = static_cast<Device*>(refCon);
	(void)flags;
	(void)stamp;
	(void)bus;
	device->RenderInCallback(frames, data);
	return noErr;
}

} // namespace

void Device::RenderInCallback(uint32_t frames, void* bufferList) {
	auto* data = static_cast<AudioBufferList*>(bufferList);

	const uint64_t now = mach_absolute_time();
	static mach_timebase_info_data_t timebase = [] {
		mach_timebase_info_data_t base{};
		mach_timebase_info(&base);
		return base;
	}();

	// Interval between callbacks, and the time spent inside the mixer.  Both are
	// appended to arrays sized in Open(); when they fill up we stop recording
	// rather than allocate.
	if (lastTick_ != 0 && intervalMs_.size() < kMaxSamples) {
		intervalMs_.push_back(TicksToSeconds(now - lastTick_, timebase) * 1000.0);
	} else if (lastTick_ != 0) {
		stats_.dropped++;
	}
	lastTick_ = now;

	// An underrun is the decoder not having produced anything by the time the
	// device asked; the callback still has to return audio, so it returns
	// silence.  Skip the first callbacks, where the ring is legitimately empty.
	const bool warm = stats_.callbacks >= 5;
	if (warm && engine_ && engine_->BufferedFrames() == 0) stats_.underruns++;

	const uint64_t workStart = mach_absolute_time();
	if (data->mNumberBuffers >= 1) {
		auto* out = static_cast<float*>(data->mBuffers[0].mData);
		const size_t samples = (size_t)frames * 2;
		if (out && data->mBuffers[0].mDataByteSize >= samples * sizeof(float)) {
			// Engine::Mix zeroes the buffer, sums every stream, and limits.  This is
			// the only work the audio thread does.
			engine_->Mix(out, (int)frames);
		} else if (out) {
			std::memset(out, 0, data->mBuffers[0].mDataByteSize);
		}
		// Non-interleaved layouts should not happen (we ask for interleaved), but
		// silence is better than garbage if a device insists.
		for (UInt32 b = 1; b < data->mNumberBuffers; b++)
			if (data->mBuffers[b].mData) std::memset(data->mBuffers[b].mData, 0, data->mBuffers[b].mDataByteSize);
	}
	const uint64_t workEnd = mach_absolute_time();
	if (workUs_.size() < kMaxSamples) workUs_.push_back(TicksToSeconds(workEnd - workStart, timebase) * 1e6);

	stats_.callbacks++;
	stats_.frames += frames;
}

// ---------------------------------------------------------------------------
// open / start / stop
// ---------------------------------------------------------------------------
bool Device::Open(Engine* engine, const DeviceTarget& target, std::string* reason) {
	if (!engine) {
		if (reason) *reason = "no mixer to connect";
		mode_ = Mode::Failed;
		return false;
	}
	if (!target.available) {
		if (reason) *reason = target.reason.empty() ? "no output device" : target.reason;
		fallbackReason_ = reason ? *reason : std::string();
		mode_ = Mode::Failed;
		return false;
	}

	engine_ = engine;
	deviceName_ = target.name;
	sampleRate_ = target.sampleRate;
	channels_ = target.channels;
	intervalMs_.clear();
	workUs_.clear();
	intervalMs_.reserve(kMaxSamples);
	workUs_.reserve(kMaxSamples);
	stats_ = CallbackStats{};

	AudioComponentDescription description{};
	description.componentType = kAudioUnitType_Output;
	description.componentSubType = kAudioUnitSubType_DefaultOutput;
	description.componentManufacturer = kAudioUnitManufacturer_Apple;
	AudioComponent component = AudioComponentFindNext(nullptr, &description);
	if (!component) {
		if (reason) *reason = "system has no default-output AudioUnit";
		fallbackReason_ = *reason;
		mode_ = Mode::Failed;
		return false;
	}

	AudioComponentInstance instance = nullptr;
	OSStatus status = AudioComponentInstanceNew(component, &instance);
	if (status != noErr || !instance) {
		if (reason) *reason = "AudioComponentInstanceNew failed (" + std::to_string((int)status) + ")";
		fallbackReason_ = *reason;
		mode_ = Mode::Failed;
		return false;
	}

	// Interleaved float32 stereo at the device's own rate.  Asking for the
	// device's rate means CoreAudio does no resampling behind our back: the
	// mixer's own resampler is what converts 44100 Hz game audio.
	AudioStreamBasicDescription format{};
	format.mSampleRate = sampleRate_;
	format.mFormatID = kAudioFormatLinearPCM;
	format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
	format.mChannelsPerFrame = (UInt32)channels_;
	format.mBitsPerChannel = 32;
	format.mFramesPerPacket = 1;
	format.mBytesPerFrame = 4 * (UInt32)channels_;
	format.mBytesPerPacket = format.mBytesPerFrame;
	status = AudioUnitSetProperty(instance, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format,
	                              sizeof format);
	if (status != noErr) {
		if (reason) *reason = "cannot set output stream format (" + std::to_string((int)status) + ")";
		fallbackReason_ = *reason;
		AudioComponentInstanceDispose(instance);
		mode_ = Mode::Failed;
		return false;
	}

	AURenderCallbackStruct callback{};
	callback.inputProc = RenderCallback;
	callback.inputProcRefCon = this;
	status = AudioUnitSetProperty(instance, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback,
	                              sizeof callback);
	if (status != noErr) {
		if (reason) *reason = "cannot install render callback (" + std::to_string((int)status) + ")";
		fallbackReason_ = *reason;
		AudioComponentInstanceDispose(instance);
		mode_ = Mode::Failed;
		return false;
	}

	status = AudioUnitInitialize(instance);
	if (status != noErr) {
		if (reason) *reason = "AudioUnitInitialize failed (" + std::to_string((int)status) + ")";
		fallbackReason_ = *reason;
		AudioComponentInstanceDispose(instance);
		mode_ = Mode::Failed;
		return false;
	}

	// How many frames the device will ask for per callback.
	UInt32 slice = 0;
	UInt32 sliceSize = sizeof(slice);
	if (AudioUnitGetProperty(instance, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &slice,
	                         &sliceSize) == noErr &&
	    slice > 0) {
		bufferFrames_ = (int)slice;
	} else {
		bufferFrames_ = target.bufferFrames;
	}

	unit_ = instance;
	mode_ = Mode::CoreAudio;
	return true;
}

bool Device::Start(std::string* reason) {
	if (mode_ != Mode::CoreAudio || !unit_) {
		if (reason) *reason = fallbackReason_.empty() ? "device is not open" : fallbackReason_;
		return false;
	}
	lastTick_ = 0;
	OSStatus status = AudioOutputUnitStart(static_cast<AudioUnit>(unit_));
	if (status != noErr) {
		if (reason) *reason = "AudioOutputUnitStart failed (" + std::to_string((int)status) + ")";
		fallbackReason_ = *reason;
		mode_ = Mode::Failed;
		return false;
	}
	running_ = true;
	return true;
}

void Device::Stop() {
	if (mode_ == Mode::CoreAudio && unit_) AudioOutputUnitStop(static_cast<AudioUnit>(unit_));
	running_ = false;
}

void Device::Close() {
	Stop();
	if (unit_) {
		AudioUnitUninitialize(static_cast<AudioUnit>(unit_));
		AudioComponentInstanceDispose(static_cast<AudioUnit>(unit_));
		unit_ = nullptr;
	}
	if (mode_ != Mode::Failed) mode_ = Mode::Closed;
	engine_ = nullptr;
	intervalMs_.clear();
	workUs_.clear();
}

void Device::RefreshStats() {
	stats_.intervalP50Ms = Percentile(intervalMs_, 0.50);
	stats_.intervalP99Ms = Percentile(intervalMs_, 0.99);
	stats_.intervalMaxMs = intervalMs_.empty() ? 0.0 : *std::max_element(intervalMs_.begin(), intervalMs_.end());

	// Jitter is the spread around the median, which is what a listener notices:
	// a device that keeps a steady period (even an unusual one) is fine, one that
	// stutters is not.
	double sum = 0.0;
	for (double v : intervalMs_) sum += (v - stats_.intervalP50Ms) * (v - stats_.intervalP50Ms);
	stats_.intervalJitterMs = intervalMs_.empty() ? 0.0 : std::sqrt(sum / (double)intervalMs_.size());

	stats_.workP50Us = Percentile(workUs_, 0.50);
	stats_.workP99Us = Percentile(workUs_, 0.99);
	stats_.workMaxUs = workUs_.empty() ? 0.0 : *std::max_element(workUs_.begin(), workUs_.end());
}

// ---------------------------------------------------------------------------
// allocation audit
// ---------------------------------------------------------------------------
int64_t Device::AuditMixerAllocations(Engine* engine, int frames, int iterations, int* worstIteration) {
	if (worstIteration) *worstIteration = -1;
	if (!engine || frames <= 0 || iterations <= 0) return -1;
	std::vector<float> buffer((size_t)frames * 2, 0.0f);

	malloc_zone_t* zone = malloc_default_zone();
	if (!zone) return -1;

	int64_t worst = 0;
	for (int i = 0; i < iterations; i++) {
		malloc_statistics_t before{};
		malloc_statistics_t after{};
		malloc_zone_statistics(zone, &before);
		engine->Mix(buffer.data(), frames);
		malloc_zone_statistics(zone, &after);
		const int64_t delta = (int64_t)after.blocks_in_use - (int64_t)before.blocks_in_use;
		if (delta > worst) {
			worst = delta;
			if (worstIteration) *worstIteration = i;
		}
	}
	return worst;
}

} // namespace audio
} // namespace artemis
