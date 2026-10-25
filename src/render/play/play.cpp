// play.cpp - the window, the clock, the input, and what a click means

#include "play.h"

#include "../../pngio.h"
#include "../screen.h"

#include <SDL.h>

#include <algorithm>
#include <cerrno>
#include <numeric>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sys/resource.h>
#include <sys/stat.h>

namespace artemis
{
	namespace
	{
		long long PeakRss()
		{
			struct rusage usage;
			if(getrusage(RUSAGE_SELF, &usage) != 0)
				return 0;
			return (long long)usage.ru_maxrss; // bytes on macOS
		}

		bool MakeParent(const std::string& path)
		{
			const size_t slash = path.rfind('/');
			if(slash == std::string::npos || slash == 0)
				return true;
			const std::string dir = path.substr(0, slash);
			return mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST;
		}

		uint64_t HashCanvas(const Canvas& canvas)
		{
			uint64_t hash = 1469598103934665603ull;
			const uint8_t* data = canvas.data();
			const size_t bytes = canvas.byteSize();
			for(size_t i = 0; i < bytes; i++)
			{
				hash ^= (uint64_t)data[i];
				hash *= 1099511628211ull;
			}
			return hash;
		}

		bool WriteWav16(const std::string& path, const std::vector<float>& interleaved, int rate,
			std::string& error)
		{
			const uint32_t frames = (uint32_t)(interleaved.size() / 2);
			const uint32_t dataBytes = frames * 4;
			FILE* file = fopen(path.c_str(), "wb");
			if(file == nullptr)
			{
				error = "cannot write " + path;
				return false;
			}
			auto put32 = [&](uint32_t value) {
				const uint8_t bytes[4] = { (uint8_t)(value & 0xff),
					(uint8_t)((value >> 8) & 0xff), (uint8_t)((value >> 16) & 0xff),
					(uint8_t)((value >> 24) & 0xff) };
				fwrite(bytes, 1, 4, file);
			};
			auto put16 = [&](uint16_t value) {
				const uint8_t bytes[2] = { (uint8_t)(value & 0xff),
					(uint8_t)((value >> 8) & 0xff) };
				fwrite(bytes, 1, 2, file);
			};
			fwrite("RIFF", 1, 4, file);
			put32(36 + dataBytes);
			fwrite("WAVE", 1, 4, file);
			fwrite("fmt ", 1, 4, file);
			put32(16);
			put16(1);
			put16(2);
			put32((uint32_t)rate);
			put32((uint32_t)rate * 4);
			put16(4);
			put16(16);
			fwrite("data", 1, 4, file);
			put32(dataBytes);
			for(float sample : interleaved)
			{
				double value = (double)sample * 32767.0;
				if(value > 32767.0)
					value = 32767.0;
				if(value < -32768.0)
					value = -32768.0;
				put16((uint16_t)(int16_t)std::lround(value));
			}
			fclose(file);
			return true;
		}

		double Percentile(std::vector<double> values, double p)
		{
			if(values.empty())
				return 0;
			std::sort(values.begin(), values.end());
			const size_t at = (size_t)std::min<double>((double)(values.size() - 1),
				std::floor(p * (double)values.size()));
			return values[at];
		}

		// Which scripted token does what.  The words are the point of the flag:
		// `--autoplay-keys "space,space,enter"` reads as what a player does.
		enum class Token
		{
			None,
			Advance, // space / enter / return / click anywhere in the message area
			Click,   // click:X:Y
			Choice,  // choice:N (1-based)
			Wait,    // wait:MS
			Quit,
		};

		struct ScriptedInput
		{
			Token token = Token::None;
			int x = 0;
			int y = 0;
			int value = 0;
			bool hasPoint = false;
		};

		ScriptedInput ParseToken(const std::string& raw)
		{
			ScriptedInput out;
			std::string text = raw;
			while(!text.empty() && (text.front() == ' ' || text.front() == '\t'))
				text.erase(text.begin());
			while(!text.empty() && (text.back() == ' ' || text.back() == '\t'))
				text.pop_back();
			if(text.empty())
				return out;
			if(text == "space" || text == "enter" || text == "return" || text == "click" ||
				text == "z" || text == "button0")
			{
				out.token = Token::Advance;
				return out;
			}
			if(text == "quit")
			{
				out.token = Token::Quit;
				return out;
			}
			if(text.compare(0, 6, "click:") == 0)
			{
				out.token = Token::Click;
				out.hasPoint = true;
				if(sscanf(text.c_str() + 6, "%d:%d", &out.x, &out.y) != 2)
					out.token = Token::None;
				return out;
			}
			if(text.compare(0, 7, "choice:") == 0)
			{
				out.token = Token::Choice;
				out.value = atoi(text.c_str() + 7);
				return out;
			}
			if(text.compare(0, 5, "wait:") == 0)
			{
				out.token = Token::Wait;
				out.value = atoi(text.c_str() + 5);
				return out;
			}
			return out;
		}
	}

	struct Player::Impl
	{
		PlayOptions options;
		PlayHost* host = nullptr;

		SDL_Window* window = nullptr;
		SDL_Renderer* renderer = nullptr;
		SDL_Texture* texture = nullptr;
		SDL_AudioDeviceID device = 0;
		bool audioDevice = false;
		std::string audioFallback;
		int audioFreq = 0;
		std::string videoDriver;
		std::string audioDriver;

		Canvas canvas;
		std::vector<ScriptedInput> script;
		size_t scriptAt = 0;
		bool quit = false;

		PlayStats stats;
		std::vector<double> frameTimes;
		std::vector<double> latencies;

		// the page on screen
		std::string fullText;
		std::string speaker;
		int64_t revealStart = 0;
		int64_t revealMs = 0;
		bool revealed = true;
		int64_t clickedAt = -1;
		int64_t lastAdvanceAt = -1000;
		int stallFrames = 0;

		Impl() : canvas(1, 1) {}

		static void AudioCallback(void* userdata, Uint8* stream, int length)
		{
			Player::Impl* self = (Player::Impl*)userdata;
			const int frames = length / (int)(sizeof(float) * 2);
			std::memset(stream, 0, (size_t)length);
			if(self->host != nullptr)
				self->host->MixAudio((float*)(void*)stream, frames);
			self->stats.audioFramesMixed += frames;
		}

		bool OpenAudio(std::string& error);
		void CloseAudio();
		bool Shot(const std::string& path, std::string& error);
		void HandleKey(int keycode);
		bool PumpScripted(int64_t nowMs);
		PlayStats RunLoop(std::string& error);
	};

	bool Player::Impl::OpenAudio(std::string& error)
	{
		error.clear();
		if(!options.audio)
			return true;
		if(SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
		{
			audioFallback = std::string("no audio subsystem: ") + SDL_GetError();
			return true;
		}
		SDL_AudioSpec want;
		std::memset(&want, 0, sizeof want);
		want.freq = options.audioRate;
		want.format = AUDIO_F32SYS;
		want.channels = 2;
		want.samples = (Uint16)options.audioFrames;
		want.callback = &Player::Impl::AudioCallback;
		want.userdata = this;
		SDL_AudioSpec have;
		std::memset(&have, 0, sizeof have);
		const char* driver = SDL_GetCurrentAudioDriver();
		audioDriver = driver == nullptr ? "(none)" : driver;
		device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
		if(device == 0)
		{
			// A machine with no sound card still has to be able to run the game and
			// be checked.  The reason is kept and printed; the loop then mixes the
			// same callback into a WAV instead of into a device.
			audioFallback = std::string("no audio device (") + audioDriver +
				"): " + SDL_GetError();
			SDL_QuitSubSystem(SDL_INIT_AUDIO);
			if(SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
				return true;
			const char* dummy = SDL_GetCurrentAudioDriver();
			audioDriver = dummy == nullptr ? "(none)" : dummy;
			if(have.format != 0 && have.format != AUDIO_F32SYS)
				audioFallback += "; the device wanted a format this mixer does not write";
			return true;
		}
		if(have.format != AUDIO_F32SYS)
		{
			audioFallback = "the device does not take float32: falling back to mixing in "
				"the loop";
			SDL_CloseAudioDevice(device);
			device = 0;
			return true;
		}
		audioDevice = true;
		audioFreq = have.freq;
		SDL_PauseAudioDevice(device, 0); // start pulling
		return true;
	}

	void Player::Impl::CloseAudio()
	{
		if(device != 0)
		{
			SDL_CloseAudioDevice(device);
			device = 0;
		}
		audioDevice = false;
	}

	bool Player::Impl::Shot(const std::string& path, std::string& error)
	{
		if(path.empty())
			return true;
		if(!MakeParent(path))
		{
			error = "cannot create the directory for " + path;
			return false;
		}
		return WritePng(path, canvas.width(), canvas.height(), canvas.data(), error);
	}

	void Player::Impl::HandleKey(int keycode)
	{
		// Only a fresh key-down is an advance; a held key produces one event, and
		// the debounce below keeps two events in one frame from becoming two pages.
		switch(keycode)
		{
		case SDLK_SPACE:
		case SDLK_RETURN:
		case SDLK_KP_ENTER:
		case SDLK_z:
			stats.keys++;
			break;
		default:
			break;
		}
	}

	bool Player::Impl::PumpScripted(int64_t nowMs)
	{
		if(scriptAt >= script.size())
			return false;
		const ScriptedInput& token = script[scriptAt];
		if(token.token == Token::None)
		{
			scriptAt++;
			return true;
		}
		if(token.token == Token::Wait)
		{
			// a scripted pause is a *virtual* pause: it moves the clock, it does not
			// sleep, so a headless run stays fast and repeatable
			revealStart -= token.value;
			scriptAt++;
			return true;
		}
		if(token.token == Token::Quit)
		{
			quit = true;
			scriptAt++;
			return true;
		}
		// the rest need the loop to be waiting for a decision
		if(!revealed)
		{
			// a click while the page is still typing: finish the page, do not skip it
			revealed = true;
			revealStart = nowMs - revealMs;
			stats.completedReveals++;
			stats.clicks++;
			clickedAt = nowMs;
			scriptAt++;
			return true;
		}
		if(stats.choices > 0 && token.token == Token::Choice)
		{
			int index = token.value - 1;
			if(index < 0 || index >= host->ChoiceCount())
				index = 0;
			host->Choose(index);
			stats.chosen = token.value;
			stats.choices--;
			clickedAt = nowMs;
			scriptAt++;
			return true;
		}
		if(stats.choices > 0 && (token.token == Token::Advance || token.token == Token::Click))
		{
			host->Choose(0);
			stats.chosen = 1;
			stats.choices--;
			clickedAt = nowMs;
			scriptAt++;
			return true;
		}
		if(stats.idleSeen && token.token == Token::Click && token.hasPoint)
		{
			for(int i = 0; i < host->RegionCount(); i++)
			{
				int left = 0, top = 0, width = 0, height = 0;
				if(!host->RegionRect(i, left, top, width, height))
					continue;
				if(token.x >= left && token.y >= top && token.x < left + width &&
					token.y < top + height && host->RegionHit(i, token.x, token.y))
				{
					host->ActivateRegion(i);
					stats.clicks++;
					clickedAt = nowMs;
					scriptAt++;
					return true;
				}
			}
			// not a hit yet: try again next frame, but do not spin forever on a
			// click that will never land
			if(++stallFrames > 240)
			{
				stats.stalledTokens++;
				stallFrames = 0;
				scriptAt++;
			}
			return true;
		}
		if(stats.idleSeen && (token.token == Token::Advance || token.token == Token::Click))
		{
			// in the idle state a plain click means "the obvious button": the first
			// region, which on the title screen is the start button
			for(int i = 0; i < host->RegionCount(); i++)
			{
				int left = 0, top = 0, width = 0, height = 0;
				if(!host->RegionRect(i, left, top, width, height))
					continue;
				if(host->RegionHit(i, left + width / 2, top + height / 2))
				{
					host->ActivateRegion(i);
					stats.clicks++;
					clickedAt = nowMs;
					scriptAt++;
					return true;
				}
			}
			if(++stallFrames > 240)
			{
				fprintf(stderr, "play: scripted \"%s\" found nothing to click after 240 "
					"frames (%d region(s))\n", "click", host->RegionCount());
				stats.stalledTokens++;
				stallFrames = 0;
				scriptAt++;
			}
			return true;
		}
		// a page that is fully shown: advance
		if(nowMs - lastAdvanceAt < 60)
			return true; // the debounce: no double-advance
		stats.advances++;
		stats.clicks++;
		clickedAt = nowMs;
		lastAdvanceAt = nowMs;
		scriptAt++;
		return true;
	}

	Player::Player() : m_impl(new Impl()) {}
	Player::~Player() { Close(); }

	bool Player::Open(const PlayOptions& options, PlayHost& host, std::string& error)
	{
		m_impl->options = options;
		m_impl->host = &host;
		m_impl->canvas = Canvas(options.width, options.height);
		for(const std::string& token : options.script)
			m_impl->script.push_back(ParseToken(token));

		if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
		{
			error = std::string("cannot start SDL: ") + SDL_GetError();
			return false;
		}
		const char* driver = SDL_GetCurrentVideoDriver();
		m_impl->videoDriver = driver == nullptr ? "(none)" : driver;

		const Uint32 flags = SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI;
		m_impl->window = SDL_CreateWindow("Artemis", SDL_WINDOWPOS_CENTERED,
			SDL_WINDOWPOS_CENTERED, options.width, options.height, flags);
		if(m_impl->window == nullptr)
		{
			error = std::string("cannot create the window: ") + SDL_GetError();
			return false;
		}
		Uint32 rendererFlags = SDL_RENDERER_ACCELERATED;
		if(options.vsync)
			rendererFlags |= SDL_RENDERER_PRESENTVSYNC;
		m_impl->renderer = SDL_CreateRenderer(m_impl->window, -1, rendererFlags);
		if(m_impl->renderer == nullptr)
		{
			// the dummy driver has no accelerated renderer; a software one is still a
			// real renderer, and the picture comes from the canvas either way
			m_impl->renderer = SDL_CreateRenderer(m_impl->window, -1, SDL_RENDERER_SOFTWARE);
		}
		if(m_impl->renderer == nullptr)
		{
			error = std::string("cannot create the renderer: ") + SDL_GetError();
			return false;
		}
		m_impl->texture = SDL_CreateTexture(m_impl->renderer, SDL_PIXELFORMAT_RGBA32,
			SDL_TEXTUREACCESS_STREAMING, options.width, options.height);
		if(m_impl->texture == nullptr)
		{
			error = std::string("cannot create the texture: ") + SDL_GetError();
			return false;
		}
		if(!m_impl->OpenAudio(error))
			return false;
		if(!m_impl->audioFallback.empty())
			fprintf(stderr, "agame: audio: %s\n", m_impl->audioFallback.c_str());
		return true;
	}

	void Player::Close()
	{
		if(m_impl == nullptr)
			return;
		m_impl->CloseAudio();
		if(m_impl->texture != nullptr)
			SDL_DestroyTexture(m_impl->texture);
		if(m_impl->renderer != nullptr)
			SDL_DestroyRenderer(m_impl->renderer);
		if(m_impl->window != nullptr)
			SDL_DestroyWindow(m_impl->window);
		m_impl->texture = nullptr;
		m_impl->renderer = nullptr;
		m_impl->window = nullptr;
		if(SDL_WasInit(SDL_INIT_VIDEO) != 0)
			SDL_QuitSubSystem(SDL_INIT_VIDEO);
		SDL_Quit();
	}

	void Player::MixFromDevice(float* out, int frames)
	{
		if(m_impl->host != nullptr)
			m_impl->host->MixAudio(out, frames);
		m_impl->stats.audioFramesMixed += frames;
	}

	PlayStats Player::Run(std::string& error)
	{
		return m_impl->RunLoop(error);
	}

	PlayStats Player::Impl::RunLoop(std::string& error)
	{
		const int64_t frameMs = options.fps > 0 ? (1000 / options.fps) : 16;
		const bool virtualClock = !options.realTime;
		int64_t now = 0;
		bool need = true;
		bool pageOnScreen = false;
		int64_t startWall = (int64_t)((double)SDL_GetPerformanceCounter() * 1000.0 /
			(double)SDL_GetPerformanceFrequency());
		int64_t lastFrameWall = startWall;
		std::vector<float> audioBlock((size_t)options.audioFrames * 2, 0.0f);
		std::vector<float> capture;

		while(!quit)
		{
			if(stats.frames >= options.maxFrames)
			{
				stats.budgetHit = true;
				quit = true;
				continue;
			}
			const int64_t frameWallStart = (int64_t)((double)SDL_GetPerformanceCounter() *
				1000.0 / (double)SDL_GetPerformanceFrequency());

			// --- input ---------------------------------------------------------
			SDL_Event event;
			while(SDL_PollEvent(&event))
			{
				if(event.type == SDL_QUIT)
					quit = true;
				else if(event.type == SDL_KEYDOWN)
				{
					if(event.key.keysym.sym == SDLK_ESCAPE)
						quit = true;
					else
						HandleKey(event.key.keysym.sym);
				}
				else if(event.type == SDL_MOUSEBUTTONDOWN &&
					event.button.button == SDL_BUTTON_LEFT)
				{
					const int x = event.button.x;
					const int y = event.button.y;
					if(stats.choices > 0)
					{
						for(int i = 0; i < host->ChoiceCount(); i++)
						{
							int left = 0, top = 0, width = 0, height = 0;
							if(!host->ChoiceRect(i, left, top, width, height))
								continue;
							if(x >= left && y >= top && x < left + width && y < top + height)
							{
								host->Choose(i);
								stats.chosen = i + 1;
								stats.choices--;
								clickedAt = now;
								break;
							}
						}
					}
					else if(stats.idleSeen)
					{
						for(int i = 0; i < host->RegionCount(); i++)
							if(host->RegionHit(i, x, y))
							{
								host->ActivateRegion(i);
								stats.clicks++;
								clickedAt = now;
								break;
							}
					}
					else if(revealed)
					{
						if(now - lastAdvanceAt >= 60)
						{
							stats.advances++;
							stats.clicks++;
							clickedAt = now;
							lastAdvanceAt = now;
							need = true;
						}
					}
					else
					{
						revealed = true;
						revealStart = now - revealMs;
						stats.completedReveals++;
						stats.clicks++;
						clickedAt = now;
					}
				}
			}
			PumpScripted(now);

			// --- the script ----------------------------------------------------
			if(need)
			{
				need = false;
				const PlayHost::Step step = host->Next();
				if(step.kind == PlayHost::Step::Kind::Page)
				{
					fullText = step.text;
					speaker = step.speaker;
					revealMs = host->RevealMs(fullText);
					revealStart = now;
					revealed = revealMs <= 0;
					pageOnScreen = true;
					stats.pages++;
					stats.idleSeen = stats.idleSeen; // stays true once the title was seen
					if(clickedAt >= 0)
					{
						latencies.push_back((double)(now - clickedAt));
						clickedAt = -1;
					}
					if(stats.choices == 0 && stats.titleShot.empty() && !options.shotTitle.empty())
					{
						// no idle screen in this script: the first page is the picture
						stats.titleShot = options.shotTitle;
					}
					if(!options.shotScene.empty() && stats.sceneShot.empty() &&
						stats.idleSeen)
						stats.sceneShot = options.shotScene;
					if(!options.shotDir.empty())
					{
						char path[1024];
						snprintf(path, sizeof path, "%s/page%03d.png", options.shotDir.c_str(),
							stats.pages);
						host->Compose(canvas, now);
						std::string shotError;
						if(!Shot(path, shotError))
						{
							error = shotError;
							return stats;
						}
						stats.shots.push_back(path);
					}
					if(options.quitAfterPages > 0 && stats.pages >= options.quitAfterPages)
						quit = true;
				}
				else if(step.kind == PlayHost::Step::Kind::Idle)
				{
					// the script parked itself on a screen with buttons: draw it and
					// wait for a click on a region
					stats.idleSeen = true;
					pageOnScreen = false;
					if(!options.shotTitle.empty() && stats.titleShot.empty())
					{
						host->Compose(canvas, now);
						std::string shotError;
						if(!Shot(options.shotTitle, shotError))
						{
							error = shotError;
							return stats;
						}
						stats.titleShot = options.shotTitle;
					}
				}
				else if(step.kind == PlayHost::Step::Kind::End)
				{
					stats.ended = true;
					quit = true;
					continue;
				}
				else
				{
					error = step.error;
					return stats;
				}
			}

			// --- the picture ---------------------------------------------------
			const int64_t revealAge = revealed ? revealMs : (now - revealStart);
			const std::string shown = host->Reveal(fullText, revealAge);
			if(!revealed && revealAge >= revealMs)
			{
				revealed = true;
				if(clickedAt >= 0)
				{
					latencies.push_back((double)(now - clickedAt));
					clickedAt = -1;
				}
			}
			host->SetPageText(shown, speaker);
			host->Compose(canvas, now);
			host->OnFrame(now);

			if(options.hashFrames)
				stats.frameHashes.push_back(HashCanvas(canvas));
			SDL_UpdateTexture(texture, nullptr, canvas.data(), canvas.width() * 4);
			SDL_RenderClear(renderer);
			SDL_RenderCopy(renderer, texture, nullptr, nullptr);
			SDL_RenderPresent(renderer);
			stats.regions = host->RegionCount();

			// --- the caption: what the reader would click on ---------------------
			if(stats.choices == 0 && host->ChoiceCount() > 0)
			{
				stats.choices = host->ChoiceCount();
				stats.chosen = -1;
			}
		
			// --- audio ----------------------------------------------------------
			if(!audioDevice && options.audio)
			{
				// no device: the same callback, into the loop's own block, so the run
				// still mixes and a capture still has something in it
				std::fill(audioBlock.begin(), audioBlock.end(), 0.0f);
				host->MixAudio(audioBlock.data(), options.audioFrames);
				stats.audioFramesMixed += options.audioFrames;
				if(!options.capturePath.empty())
				{
					const size_t limit = (size_t)(options.captureSeconds *
						(double)options.audioRate) * 2;
					if(capture.size() < limit)
						capture.insert(capture.end(), audioBlock.begin(), audioBlock.end());
				}
			}

			// --- the clock ------------------------------------------------------
			const int64_t frameWallEnd = (int64_t)((double)SDL_GetPerformanceCounter() *
				1000.0 / (double)SDL_GetPerformanceFrequency());
			const double frameUsed = (double)(frameWallEnd - frameWallStart);
			frameTimes.push_back(frameUsed);
			stats.frames++;
			if(virtualClock)
			{
				now += frameMs;
			}
			else
			{
				const int64_t target = frameWallStart + frameMs;
				int64_t waitMs = target - frameWallEnd;
				if(waitMs > 0)
					SDL_Delay((Uint32)waitMs);
				now = frameWallEnd - startWall;
			}
			lastFrameWall = frameWallEnd;
		}

		const int64_t endWall = (int64_t)((double)SDL_GetPerformanceCounter() * 1000.0 /
			(double)SDL_GetPerformanceFrequency());
		stats.wallMs = (double)(endWall - startWall);
		stats.videoDriver = videoDriver;
		stats.audioDriver = audioDriver;
		stats.audioDevice = audioDevice;
		stats.audioFallback = audioFallback;
		stats.audioFreq = audioFreq > 0 ? audioFreq : (audioDevice ? options.audioRate : 0);
		stats.frameAvgMs = frameTimes.empty() ? 0
			: std::accumulate(frameTimes.begin(), frameTimes.end(), 0.0) /
				(double)frameTimes.size();
		stats.frameP50Ms = Percentile(frameTimes, 0.50);
		stats.frameP95Ms = Percentile(frameTimes, 0.95);
		stats.frameMaxMs = frameTimes.empty() ? 0
			: *std::max_element(frameTimes.begin(), frameTimes.end());
		if(!latencies.empty())
		{
			stats.latencyAvgMs = std::accumulate(latencies.begin(), latencies.end(), 0.0) /
				(double)latencies.size();
			stats.latencyMaxMs = *std::max_element(latencies.begin(), latencies.end());
			stats.latencySamples = (int)latencies.size();
		}
		stats.realTime = options.realTime;
		stats.capturePath = options.capturePath;
		stats.captureSeconds = options.captureSeconds;
		if(!capture.empty() && !options.capturePath.empty())
		{
			if(!MakeParent(options.capturePath))
			{
				// a capture that cannot be written must not fail the run
				stats.audioFallback += "; capture path is not writable";
			}
			else
			{
				std::string captureError;
				stats.captured = WriteWav16(options.capturePath, capture, options.audioRate,
					captureError);
				if(!stats.captured)
					stats.audioFallback += "; " + captureError;
			}
		}
		stats.peakRss = PeakRss();
		stats.ok = error.empty();
		return stats;
	}
}
