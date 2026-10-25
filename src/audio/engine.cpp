// engine.cpp - the mixer `agame` talks to
//
// Threading, which is the whole point of this file:
//
//   render thread      Engine::Mix()   drains per-stream rings, no locks, no I/O
//   decoder thread     one worker       tops up every active stream's ring
//   control thread     PlayBgm() etc.  opens files, parses .sli, fills slots
//
// Opening a file and decoding its headers takes milliseconds, so it happens on
// the control thread (a script tag), never inside Mix().  Slots are published
// through atomics; the objects they point at are owned by the engine's cache
// and live until Shutdown(), so a pointer read by Mix() can never dangle even
// while the decoder thread is retiring that slot.
//
// Asset names come from the scripts: `[bgm file="moon"]` means moon.ogg plus
// the sibling text member moon.ogg.sli.  Scripts say the stem, the archive
// holds the directories, so Resolve() tries the same set of directories a
// shipped title uses.

#include "audio.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace artemis {
namespace audio {

namespace {

// Enough slots for a crossfading BGM, the SE bank and one voice line.
constexpr int kMaxActive = 1 /*bgm*/ + 16 /*se*/ + 1 /*voice*/ + 2 /*retiring*/;

bool FileExists(const std::string& path) {
	std::FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return false;
	std::fclose(f);
	return true;
}

std::string JoinPath(const std::string& a, const std::string& b) {
	if (a.empty()) return b;
	if (a.back() == '/') return a + b;
	return a + "/" + b;
}

bool HasExtension(const std::string& name) {
	size_t dot = name.rfind('.');
	size_t slash = name.find_last_of("/\\");
	return dot != std::string::npos && (slash == std::string::npos || dot > slash);
}

} // namespace

// ---------------------------------------------------------------------------
// Engine::Impl
// ---------------------------------------------------------------------------
struct Engine::Impl {
	Config config;
	std::atomic<Stream*> active[kMaxActive];
	Group slotGroup[kMaxActive];
	float slotBaseVolume[kMaxActive];

	// Ownership.  Cached streams live until Shutdown, which is what makes the
	// raw pointers above safe to read from the mixer thread.
	std::map<std::string, std::shared_ptr<Stream>> cache;

	// Control-plane bookkeeping (only touched by the control thread).
	int bgmSlot = -1;
	int voiceSlot = -1;
	int nextSeSlot = 0;

	std::thread worker;
	std::mutex control; // guards cache + Start/Fill handover
	std::mutex workerMutex;
	std::condition_variable workerWake;
	bool running = false;

	std::atomic<int64_t>* framesDecoded = nullptr;

	Impl() {
		for (int i = 0; i < kMaxActive; i++) {
			active[i].store(nullptr, std::memory_order_relaxed);
			slotGroup[i] = GroupBgm;
			slotBaseVolume[i] = 1.0f;
		}
	}

	int Publish(Stream* stream, Group group, float baseVolume) {
		// The same Stream object serves every play of that file (the cache
		// hands out one per path), so two slots must never share it: that
		// would double its audio and let one slot retire the other's stream.
		for (int i = 0; i < kMaxActive; i++) {
			if (active[i].load(std::memory_order_acquire) == stream)
				active[i].store(nullptr, std::memory_order_release);
		}
		for (int i = 0; i < kMaxActive; i++) {
			Stream* expected = nullptr;
			if (active[i].compare_exchange_strong(expected, stream, std::memory_order_acq_rel)) {
				slotGroup[i] = group;
				slotBaseVolume[i] = baseVolume;
				workerWake.notify_all();
				return i;
			}
		}
		return -1;
	}

	void Retire(int slot) { active[slot].store(nullptr, std::memory_order_release); }

	// The directory list a shipped Artemis title uses for audio members.
	void ResolveCandidates(const std::string& name, std::vector<std::string>* out) const {
		static const char* const kDirs[] = {"", "bgm", "se", "voice", "vo", "sound/bgm", "sound/se", "sound/vo"};
		std::vector<std::string> stems;
		if (HasExtension(name)) {
			stems.push_back(name);
		} else {
			stems.push_back(name + ".ogg");
			stems.push_back(name);
		}
		for (const std::string& stem : stems)
			for (const char* dir : kDirs) out->push_back(JoinPath(JoinPath(config.assetRoot, dir), stem));
	}

	bool Resolve(const std::string& name, std::string* path) const {
		std::vector<std::string> candidates;
		ResolveCandidates(name, &candidates);
		for (const std::string& c : candidates) {
			if (FileExists(c)) {
				*path = c;
				return true;
			}
		}
		return false;
	}

	// Stream cache: one Stream per file, reused across plays so repeated
	// `[se]` effects do not pay for reopening the container every time.
	std::shared_ptr<Stream> GetStream(const std::string& path, std::string* error) {
		auto it = cache.find(path);
		if (it != cache.end()) return it->second;
		Stream::Config cfg;
		cfg.engineRate = config.sampleRate > 0 ? config.sampleRate : 44100;
		cfg.volume = 1.0f;
		auto stream = Stream::Open(path, cfg, error);
		if (!stream) return nullptr;
		cache[path] = stream;
		return stream;
	}

	void WorkerLoop() {
		std::unique_lock<std::mutex> lock(workerMutex);
		while (running) {
			bool worked = false;
			for (int i = 0; i < kMaxActive && running; i++) {
				Stream* s = active[i].load(std::memory_order_acquire);
				if (!s) continue;
				if (s->Stopped()) {
					Retire(i);
					continue;
				}
				const int got = s->Fill();
				if (got > 0) {
					framesDecoded->fetch_add(got, std::memory_order_relaxed);
					worked = true;
				} else if (s->Finished() && s->Buffered() == 0) {
					// Drain then disappear: the mixer must be allowed to play
					// out whatever is still in the ring.
					Retire(i);
				}
			}
			if (!worked) workerWake.wait_for(lock, std::chrono::milliseconds(2));
		}
	}

	// Frames currently buffered across every sounding stream.  Not needed by
	// the audio callback; it lets a headless caller pace itself the way a real
	// device would, instead of draining the mixer faster than it can decode.
	int BufferedFrames() const {
		int most = 0;
		for (int i = 0; i < kMaxActive; i++) {
			Stream* s = active[i].load(std::memory_order_acquire);
			if (s) most = std::max(most, s->Buffered());
		}
		return most;
	}

};

Engine::Engine() : impl_(std::make_unique<Impl>()) {
	for (int i = 0; i < GroupCount; i++) volumes_[i].store(1.0f);
}

Engine::~Engine() { Shutdown(); }

bool Engine::Open(const Config& cfg, std::string* error) {
	impl_->config = cfg;
	impl_->framesDecoded = &framesDecoded_;
	framesDecoded_.store(0);
	if (impl_->config.sampleRate <= 0) impl_->config.sampleRate = 44100;
	impl_->running = true;
	impl_->worker = std::thread([this] { impl_->WorkerLoop(); });
	(void)error;
	return true;
}

void Engine::Shutdown() {
	if (!impl_ || !impl_->running) return;
	{
		std::lock_guard<std::mutex> lock(impl_->workerMutex);
		impl_->running = false;
	}
	impl_->workerWake.notify_all();
	if (impl_->worker.joinable()) impl_->worker.join();
	for (int i = 0; i < kMaxActive; i++) impl_->Retire(i);
	impl_->cache.clear();
}

// ---------------------------------------------------------------------------
// BGM
// ---------------------------------------------------------------------------
bool Engine::PlayBgm(const std::string& name, bool loop, int fadeMs) {
	return PlayBgmWithSli(name, std::string(), loop, fadeMs);
}

bool Engine::PlayBgmWithSli(const std::string& name, const std::string& sliText, bool loop, int fadeMs) {
	std::string path;
	if (!impl_->Resolve(name, &path)) return false;

	std::string error;
	std::shared_ptr<Stream> stream;
	{
		std::lock_guard<std::mutex> lock(impl_->control);
		stream = impl_->GetStream(path, &error);
	}
	if (!stream) return false;

	// Loop window: a .sli beside the ogg first, then explicitly supplied text
	// (which is how an archive member reaches us), else the whole file.
	SliInfo sli;
	bool haveSli = false;
	if (!sliText.empty()) {
		haveSli = sli.Parse(sliText, nullptr);
	} else if (FileExists(path + ".sli")) {
		haveSli = sli.Load(path + ".sli", nullptr);
	}

	LoopPlan plan;
	if (haveSli) {
		sli.unit = ResolveSliUnit(sli, stream->Info().frames, stream->Info().sampleRate);
		plan = LoopPlan::FromSli(sli, stream->Info().frames, stream->Info().sampleRate);
	} else {
		plan = LoopPlan::Whole(stream->Info().frames);
	}
	plan.looping = loop;
	if (!loop) {
		plan.introEnd = stream->Info().frames;
		plan.loopStart = 0;
		plan.loopEnd = stream->Info().frames;
	}

	// Crossfade: let the outgoing BGM ramp to silence and keep sounding until
	// it is done, which is why retiring streams get their own slots.
	const float volume = Volume(GroupBgm);
	Stream* previous = nullptr;
	if (impl_->bgmSlot >= 0) {
		previous = impl_->active[impl_->bgmSlot].load(std::memory_order_acquire);
		if (previous == stream.get()) previous = nullptr;
	}

	{
		std::lock_guard<std::mutex> lock(impl_->control);
		stream->Start(plan);
	}
	// After Start(), not before: Start() latches the current fade generation, so
	// a fade requested first would be swallowed and the track would appear at
	// full volume (this was a real bug caught by the crossfade assertion).
	// A fresh stream carries whatever gain the previous track left behind, so a
	// fade-in has to state its starting point explicitly: silence.
	if (fadeMs > 0)
		stream->FadeFromTo(0.0f, volume, fadeMs);
	else
		stream->FadeTo(volume, 0);
	impl_->bgmSlot = impl_->Publish(stream.get(), GroupBgm, volume);
	currentBgm_ = name;

	if (previous) {
		previous->FadeTo(0.0f, fadeMs > 0 ? fadeMs : 0);
		if (fadeMs <= 0) previous->Stop();
	}
	return true;
}

bool Engine::StopBgm(int fadeMs) {
	if (impl_->bgmSlot < 0) return false;
	Stream* s = impl_->active[impl_->bgmSlot].load(std::memory_order_acquire);
	if (!s) return false;
	if (fadeMs > 0) s->FadeTo(0.0f, fadeMs);
	else s->Stop();
	impl_->bgmSlot = -1;
	currentBgm_.clear();
	return true;
}

// ---------------------------------------------------------------------------
// SE / voice
// ---------------------------------------------------------------------------
bool Engine::PlaySe(const std::string& name, int channel) {
	std::string path;
	if (!impl_->Resolve(name, &path)) return false;
	std::string error;
	std::shared_ptr<Stream> stream;
	{
		std::lock_guard<std::mutex> lock(impl_->control);
		stream = impl_->GetStream(path, &error);
	}
	if (!stream) return false;

	const float volume = Volume(GroupSe);
	stream->FadeTo(volume, 0);
	{
		std::lock_guard<std::mutex> lock(impl_->control);
		// A one-shot: no looping.
		LoopPlan once = LoopPlan::Whole(stream->Info().frames);
		once.looping = false;
		stream->Start(once);
	}
	const int slot = impl_->Publish(stream.get(), GroupSe, volume);
	if (slot < 0) return false;
	if (channel >= 0)
		impl_->nextSeSlot = channel + 1;
	else
		impl_->nextSeSlot = (impl_->nextSeSlot + 1) % std::max(1, impl_->config.seChannels);
	return true;
}

bool Engine::PlayVoice(const std::string& name) {
	std::string path;
	if (!impl_->Resolve(name, &path)) return false;
	std::string error;
	std::shared_ptr<Stream> stream;
	{
		std::lock_guard<std::mutex> lock(impl_->control);
		stream = impl_->GetStream(path, &error);
	}
	if (!stream) return false;

	// A new line interrupts the previous one, which is what scripts expect.
	if (impl_->voiceSlot >= 0) {
		Stream* old = impl_->active[impl_->voiceSlot].load(std::memory_order_acquire);
		if (old && old != stream.get()) {
			old->Stop();
			impl_->Retire(impl_->voiceSlot);
		}
	}

	const float volume = Volume(GroupVoice);
	stream->FadeTo(volume, 0);
	{
		std::lock_guard<std::mutex> lock(impl_->control);
		LoopPlan once = LoopPlan::Whole(stream->Info().frames);
		once.looping = false;
		stream->Start(once);
	}
	impl_->voiceSlot = impl_->Publish(stream.get(), GroupVoice, volume);
	return impl_->voiceSlot >= 0;
}

void Engine::StopAll() {
	for (int i = 0; i < kMaxActive; i++) {
		Stream* s = impl_->active[i].load(std::memory_order_acquire);
		if (s) s->Stop();
	}
	impl_->bgmSlot = -1;
	impl_->voiceSlot = -1;
	currentBgm_.clear();
}

// ---------------------------------------------------------------------------
// volume
// ---------------------------------------------------------------------------
void Engine::SetVolume(Group group, float volume) {
	if ((int)group < 0 || (int)group >= GroupCount) return;
	volume = std::max(0.0f, std::min(1.0f, volume));
	volumes_[group].store(volume);
	// Re-derive each sounding stream's gain so the change is audible at once.
	for (int i = 0; i < kMaxActive; i++) {
		Stream* s = impl_->active[i].load(std::memory_order_acquire);
		if (!s || impl_->slotGroup[i] != group) continue;
		s->FadeTo(volume * impl_->slotBaseVolume[i], 0);
	}
}

float Engine::Volume(Group group) const {
	if ((int)group < 0 || (int)group >= GroupCount) return 1.0f;
	return volumes_[group].load();
}

// ---------------------------------------------------------------------------
// the audio callback
// ---------------------------------------------------------------------------
void Engine::Mix(float* out, int frames) {
	if (!out || frames <= 0) return;
	std::memset(out, 0, (size_t)frames * 2 * sizeof(float));
	for (int i = 0; i < kMaxActive; i++) {
		Stream* s = impl_->active[i].load(std::memory_order_acquire);
		if (s) s->Mix(out, frames);
	}
	// Soft limit rather than clip: see SoftLimit().  Ordinary mixes are below
	// the knee and come through untouched; a pile-up of sound effects gets
	// rounded off instead of flat-topped.
	for (size_t i = 0; i < (size_t)frames * 2; i++) out[i] = SoftLimit(out[i]);
	framesMixed_.fetch_add(frames, std::memory_order_relaxed);
}

float SoftLimit(float sample) {
	const float magnitude = std::fabs(sample);
	if (magnitude <= kLimiterKnee) return sample; // linear region: bit exact
	const float over = (magnitude - kLimiterKnee) / (1.0f - kLimiterKnee);
	const float limited = kLimiterKnee + (kLimiterCeiling - kLimiterKnee) * std::tanh(over);
	return sample < 0.0f ? -limited : limited;
}

int Engine::BufferedFrames() const { return impl_ ? impl_->BufferedFrames() : 0; }

int Engine::ActiveStreams() const {
	int n = 0;
	for (int i = 0; i < kMaxActive; i++)
		if (impl_->active[i].load(std::memory_order_acquire)) n++;
	return n;
}

} // namespace audio
} // namespace artemis
