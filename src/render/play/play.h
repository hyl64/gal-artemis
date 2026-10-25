// play.h - the interactive loop: a window, a clock, input, and a screenshot
//
// Everything below this file is already a picture: `Stage::Composite` draws a
// frame, `Vm` produces pages, the effect layer answers "what of this page is on
// screen at t ms", the audio layer answers "what does the mix sound like now".
// None of that needs a window.  This file is the part that does: it owns the
// SDL window, the frame clock, the input queue, the audio device and the
// screenshots, and it asks the host for everything else through PlayHost.
//
// Why a host interface and not direct calls: `artemis_render` (this library)
// links neither `artemis_audio` nor `artemis_fx`.  The front end - `agame` -
// links all three and implements this interface, so a half-written audio or
// effect layer cannot stop the render library from building, and this file stays
// readable as "the window".
//
// The behaviours that make it feel like a galgame rather than a slideshow, and
// what each one is:
//
//   * the reveal.  A page is typed out at the game's own `g.msgspeed` (30 ms per
//     glyph, out of first.iet).  A click while it is still typing *finishes the
//     page*, it does not skip it - that is the standard behaviour and it is
//     asserted in the self test.
//   * no hold-to-skip.  Advancing needs a fresh key-down or a fresh click, with a
//     debounce; a key held down produces exactly one advance.
//   * `[stop]` is not the end.  It parks the script on a screen whose `[button]`s
//     are clickable (the title screen and the chapter screen work like that), so
//     the loop waits for a hit region instead of quitting.
//   * a choice is a page with options: they are drawn by the host in the game's
//     own `select` message style, and picked with the mouse or a number key.
//
// Two clocks.  A real window runs on wall time and is paced to the game's FPS
// (vsync when the driver offers it, a sleep otherwise).  A headless run
// (`SDL_VIDEODRIVER=dummy`) uses a *virtual* clock: one frame is one 1/60 s step
// with no sleeping, so a whole run takes milliseconds and is deterministic
// enough to assert on.  The frame time and latency numbers still come from the
// same code path.

#ifndef ARTEMIS_RENDER_PLAY_H
#define ARTEMIS_RENDER_PLAY_H

#include "../screen.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace artemis
{
	// What the loop needs from the layers above it.
	class PlayHost
	{
	public:
		struct Step
		{
			enum class Kind
			{
				Page,  // words to show: the reveal starts now
				Idle,  // nothing to show, but the screen is live (a [stop] with buttons)
				End,   // the script is over
				Error, // it broke, and says how
			};
			Kind kind = Kind::End;
			std::string text;
			std::string speaker;
			std::string error;
		};

		virtual ~PlayHost() {}

		// Advance the script by one step.
		virtual Step Next() = 0;
		// Put the visible prefix on the stage (and the name box).
		virtual void SetPageText(const std::string& shown, const std::string& speaker) = 0;
		// What of `text` a reader sees `ms` after the page appeared.
		virtual std::string Reveal(const std::string& text, int64_t ms) = 0;
		// When the last glyph lands.
		virtual int64_t RevealMs(const std::string& text) = 0;
		// Draw the frame: layers, text, effects, quake.  The host owns all of it.
		virtual void Compose(Canvas& canvas, int64_t nowMs) = 0;
		// Called once per frame, after Compose, for anything that is not pixels.
		virtual void OnFrame(int64_t nowMs) = 0;
		// Whether the frame just composed has a decoded background behind the words:
		// the same rule the still-frame run uses to decide a screen is "real".
		virtual bool HasBackground() const { return true; }

		// Click regions (the title screen's buttons).
		virtual int RegionCount() const = 0;
		virtual bool RegionRect(int index, int& left, int& top, int& width, int& height) const = 0;
		virtual bool RegionHit(int index, int x, int y) const = 0;
		virtual void ActivateRegion(int index) = 0;

		// Choices, when a Select event is on screen.
		virtual int ChoiceCount() const = 0;
		virtual bool ChoiceRect(int index, int& left, int& top, int& width, int& height) const = 0;
		virtual void Choose(int index) = 0;

		// Interleaved stereo float32, `frames` frames.  Called from the audio
		// device's thread when a device is open, and from the loop when none is.
		virtual void MixAudio(float* out, int frames) = 0;

		// Real sound.  The host owns the device (CoreAudio, through the audio
		// layer), so this file needs no audio API at all: it either has a device
		// pulling MixAudio() or it mixes on its own clock.  `info` describes what
		// opened; `fallback` says why nothing did, and is printed.
		virtual bool OpenAudioDevice(std::string& info, std::string& fallback)
		{
			fallback = "this host has no audio device";
			return false;
		}
		virtual void CloseAudioDevice() {}
		virtual void AudioStats(std::vector<std::string>& out) { (void)out; }
	};

	struct PlayOptions
	{
		int width = 1024;
		int height = 698;
		int fps = 60;
		int maxPages = 100;
		bool vsync = true;
		bool realTime = false;      // false = virtual clock (headless)
		bool audio = true;
		int audioRate = 44100;
		int audioFrames = 512;      // device buffer
		std::vector<std::string> script; // scripted input tokens, see below
		std::string shotDir;        // one PNG per phase change
		std::string shotTitle;      // the idle screen's picture
		std::string shotScene;      // the first page of the body
		std::string shotBackground; // the first page that also has a background
		int quitAfterPages = 0;     // 0 = run until the script ends
		bool hashFrames = false;    // keep a hash of every frame, for a regression run
		std::string capturePath;    // WAV, written when no device is open
		double captureSeconds = 8;  // how much of the mix to keep
		// A run must end.  A script with no more input, or a click that never lands,
		// would otherwise spin forever - so every run has a frame budget, and hitting
		// it is reported rather than silently ignored.
		int maxFrames = 200000;
		bool verboseInput = false;  // print what each click did
		// A headless run has nobody to click.  When the scripted input is spent, the
		// loop ends instead of spinning - a real window keeps waiting for a person.
		bool quitWhenOutOfInput = true;
	};

	struct PlayStats
	{
		bool ok = false;
		std::string error;

		int pages = 0;
		int frames = 0;
		int clicks = 0;
		int keys = 0;
		int completedReveals = 0; // a click that finished the typing instead of advancing
		int regionClicks = 0;     // clicks that hit a [button]/[point]
		int advances = 0;
		int regions = 0;
		int choices = 0;
		int chosen = -1;
		bool idleSeen = false;
		bool ended = false;

		double wallMs = 0;
		double frameAvgMs = 0;
		double frameP50Ms = 0;
		double frameP95Ms = 0;
		double frameMaxMs = 0;
		double intervalP50Ms = 0; // wall time between the starts of two frames
		double intervalP95Ms = 0;
		int64_t lastFrameStamp = -1;
		double latencyAvgMs = 0; // click -> the next page composited
		double latencyMaxMs = 0;
		int latencySamples = 0;
		long long peakRss = 0;

		std::string videoDriver;
		std::string audioDriver;
		bool audioDevice = false;
		std::string audioFallback;
		std::string audioDeviceInfo;
		std::vector<std::string> audioStats;
		int audioFreq = 0;
		int audioSilence = 0;
		int64_t audioFramesMixed = 0;

		bool realTime = false;
		bool budgetHit = false;
		bool inputExhausted = false;
		int stalledTokens = 0;      // scripted inputs that never found anything to hit
		std::vector<uint64_t> frameHashes; // one per frame when asked for
		std::string capturePath;
		double captureSeconds = 0;
		bool captured = false;
		std::string titleShot;
		std::string sceneShot;
		std::string backgroundShot;
		std::vector<std::string> shots;
	};

	class Player
	{
	public:
		Player();
		~Player();
		Player(const Player&) = delete;
		Player& operator=(const Player&) = delete;

		bool Open(const PlayOptions& options, PlayHost& host, std::string& error);
		void Close();
		PlayStats Run(std::string& error);

		// for the audio callback
		void MixFromDevice(float* out, int frames);

	private:
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};
}

#endif // ARTEMIS_RENDER_PLAY_H
