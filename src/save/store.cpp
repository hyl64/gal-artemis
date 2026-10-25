// store.cpp - the save directory, its files and its thumbnails

#include "store.h"

#include "format.h" // SerializeState / DeserializeState / Sha1Hex

#include "../pngio.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace artemis
{
	namespace save
	{
		namespace
		{
			bool MakeDirs(const std::string& path, std::string& error)
			{
				if(path.empty())
				{
					error = "an empty save directory is not a directory";
					return false;
				}
				std::string built;
				for(size_t i = 0; i < path.size(); i++)
				{
					built.push_back(path[i]);
					const bool last = i + 1 == path.size();
					if(path[i] != '/' && !last)
						continue;
					struct stat st;
					if(stat(built.c_str(), &st) == 0)
					{
						if(!S_ISDIR(st.st_mode))
						{
							error = built + " exists and is not a directory";
							return false;
						}
						continue;
					}
					if(mkdir(built.c_str(), 0755) != 0 && errno != EEXIST)
					{
						error = "cannot create " + built + ": " + strerror(errno);
						return false;
					}
				}
				return true;
			}

			std::string HomeDir()
			{
				const char* home = getenv("HOME");
				return home ? std::string(home) : std::string(".");
			}

			uint8_t Mix(uint8_t a, uint8_t b, double t)
			{
				return (uint8_t)(a + (b - a) * t + 0.5);
			}
		}

		std::string SaveStore::DefaultRoot()
		{
			if(const char* override = getenv("ATRMIS_SAVE_DIR"))
				if(*override)
					return override;
			return HomeDir() + "/Library/Application Support/gal-engines/artemis-saves";
		}

		SaveStore::SaveStore(const std::string& root) : m_root(root)
		{
			std::string error;
			if(!MakeDirs(m_root, error))
			{
				m_ok = false;
				m_error = error;
				return;
			}
			m_ok = true;
		}

		std::string SaveStore::SlotFile(int slot) const
		{
			return m_root + "/save" + std::to_string(slot) + ".dat";
		}

		std::string SaveStore::ThumbFile(int slot) const
		{
			return m_root + "/save" + std::to_string(slot) + ".png";
		}

		std::string SaveStore::AutoFile() const { return m_root + "/autosave.dat"; }
		std::string SaveStore::AutoThumbFile() const { return m_root + "/autosave.png"; }
		std::string SaveStore::ReadMarksFile() const { return m_root + "/alreadyread.dat"; }
		std::string SaveStore::BacklogFile() const { return m_root + "/backlog.dat"; }

		bool SaveStore::HasSlot(int slot) const
		{
			struct stat st;
			return stat(SlotFile(slot).c_str(), &st) == 0;
		}

		bool SaveStore::HasAutosave() const
		{
			struct stat st;
			return stat(AutoFile().c_str(), &st) == 0;
		}

		// ------------------------------------------------------------- pictures

		Frame ScaleStretch(const Frame& frame, int width, int height)
		{
			Frame out;
			if(frame.empty() || width <= 0 || height <= 0)
				return out;
			out.width = width;
			out.height = height;
			out.rgba.assign((size_t)width * height * 4, 0);
			const bool down = width < frame.width || height < frame.height;
			for(int y = 0; y < height; y++)
			{
				for(int x = 0; x < width; x++)
				{
					uint8_t* dst = out.rgba.data() + ((size_t)y * width + x) * 4;
					if(!down)
					{
						// nearest, so an upscale is exact and repeatable
						const int sx = frame.width * x / width;
						const int sy = frame.height * y / height;
						const uint8_t* src = frame.rgba.data()
							+ ((size_t)sy * frame.width + sx) * 4;
						memcpy(dst, src, 4);
						continue;
					}
					// box filter: average every source pixel that lands in this cell
					const int x0 = frame.width * x / width;
					const int x1 = frame.width * (x + 1) / width;
					const int y0 = frame.height * y / height;
					const int y1 = frame.height * (y + 1) / height;
					const int xa = x0 < x1 ? x0 : x1;
					const int xb = (x1 > x0 ? x1 : x0 + 1);
					const int ya = y0 < y1 ? y0 : y1;
					const int yb = (y1 > y0 ? y1 : y0 + 1);
					uint64_t r = 0, g = 0, b = 0, a = 0, n = 0;
					for(int sy = ya; sy < yb && sy < frame.height; sy++)
						for(int sx = xa; sx < xb && sx < frame.width; sx++)
						{
							const uint8_t* src = frame.rgba.data()
								+ ((size_t)sy * frame.width + sx) * 4;
							r += src[0];
							g += src[1];
							b += src[2];
							a += src[3];
							n++;
						}
					if(n == 0)
						continue;
					dst[0] = (uint8_t)(r / n);
					dst[1] = (uint8_t)(g / n);
					dst[2] = (uint8_t)(b / n);
					dst[3] = (uint8_t)(a / n);
				}
			}
			return out;
		}

		bool WriteFramePng(const std::string& path, const Frame& frame, std::string& error,
			std::string* sha1)
		{
			if(frame.empty())
			{
				error = "nothing to write: the frame has no pixels";
				return false;
			}
			std::vector<uint8_t> encoded;
			if(!WritePngToMemory(encoded, frame.width, frame.height, frame.rgba.data(), error))
				return false;
			if(sha1)
				*sha1 = Sha1Hex(encoded.data(), encoded.size());
			FILE* f = fopen(path.c_str(), "wb");
			if(f == nullptr)
			{
				error = "cannot write " + path;
				return false;
			}
			const size_t wrote = fwrite(encoded.data(), 1, encoded.size(), f);
			fclose(f);
			if(wrote != encoded.size())
			{
				error = "short write to " + path;
				return false;
			}
			return true;
		}

		bool ReadFramePng(const std::string& path, Frame& frame, std::string& error)
		{
			Image image;
			if(!ReadPng(path, image, error))
				return false;
			frame.width = image.width;
			frame.height = image.height;
			frame.rgba = std::move(image.rgba);
			return true;
		}

		// ------------------------------------------------------------ writing

		bool SaveStore::WriteAtomic(const std::string& path, const std::vector<uint8_t>& bytes,
			std::string& error) const
		{
			const std::string temporary = path + ".tmp";
			FILE* f = fopen(temporary.c_str(), "wb");
			if(f == nullptr)
			{
				error = "cannot write " + temporary;
				return false;
			}
			const size_t wrote = bytes.empty() ? 0 : fwrite(bytes.data(), 1, bytes.size(), f);
			const int failed = ferror(f);
			fclose(f);
			if(wrote != bytes.size() || failed)
			{
				unlink(temporary.c_str());
				error = "short write to " + temporary;
				return false;
			}
			if(rename(temporary.c_str(), path.c_str()) != 0)
			{
				unlink(temporary.c_str());
				error = "cannot move " + temporary + " into place: " + strerror(errno);
				return false;
			}
			return true;
		}

		bool SaveStore::WriteSlot(SaveState state, int slot, const Frame* frame,
			int thumbWidth, int thumbHeight, std::string& error)
		{
			state.slot = slot;
			state.autosave = false;
			if(state.timestampMs == 0)
				state.timestampMs = (int64_t)time(nullptr) * 1000;
			if(state.displayTime.empty())
				state.displayTime = SaveState::FormatTime(state.timestampMs);
			state.thumbnail = ThumbnailInfo{};
			if(frame != nullptr && !frame->empty())
			{
				const Frame scaled = ScaleStretch(*frame, thumbWidth > 0 ? thumbWidth : 160,
					thumbHeight > 0 ? thumbHeight : 90);
				std::string sha1;
				if(!WriteFramePng(ThumbFile(slot), scaled, error, &sha1))
					return false;
				state.thumbnail.fileName = "save" + std::to_string(slot) + ".png";
				state.thumbnail.width = scaled.width;
				state.thumbnail.height = scaled.height;
				state.thumbnail.sha1 = sha1;
			}
			return WriteAtomic(SlotFile(slot), SerializeState(state), error);
		}

		bool SaveStore::WriteAutosave(SaveState state, const Frame* frame, int thumbWidth,
			int thumbHeight, std::string& error)
		{
			state.autosave = true;
			if(state.timestampMs == 0)
				state.timestampMs = (int64_t)time(nullptr) * 1000;
			if(state.displayTime.empty())
				state.displayTime = SaveState::FormatTime(state.timestampMs);
			state.thumbnail = ThumbnailInfo{};
			if(frame != nullptr && !frame->empty())
			{
				const Frame scaled = ScaleStretch(*frame, thumbWidth > 0 ? thumbWidth : 160,
					thumbHeight > 0 ? thumbHeight : 90);
				std::string sha1;
				if(!WriteFramePng(AutoThumbFile(), scaled, error, &sha1))
					return false;
				state.thumbnail.fileName = "autosave.png";
				state.thumbnail.width = scaled.width;
				state.thumbnail.height = scaled.height;
				state.thumbnail.sha1 = sha1;
			}
			return WriteAtomic(AutoFile(), SerializeState(state), error);
		}

		// ------------------------------------------------------------ reading

		bool SaveStore::ReadFileBytes(const std::string& path, std::vector<uint8_t>& out,
			std::string& error) const
		{
			FILE* f = fopen(path.c_str(), "rb");
			if(f == nullptr)
			{
				error = "no save file at " + path;
				return false;
			}
			fseek(f, 0, SEEK_END);
			const long size = ftell(f);
			fseek(f, 0, SEEK_SET);
			out.assign((size_t)(size > 0 ? size : 0), 0);
			if(size > 0)
				out.resize(fread(out.data(), 1, out.size(), f));
			fclose(f);
			return true;
		}

		bool SaveStore::ReadSlot(int slot, SaveState& out, std::string& error) const
		{
			std::vector<uint8_t> bytes;
			if(!ReadFileBytes(SlotFile(slot), bytes, error))
				return false;
			return DeserializeState(bytes.data(), bytes.size(), out, error);
		}

		bool SaveStore::ReadAutosave(SaveState& out, std::string& error) const
		{
			std::vector<uint8_t> bytes;
			if(!ReadFileBytes(AutoFile(), bytes, error))
				return false;
			return DeserializeState(bytes.data(), bytes.size(), out, error);
		}

		SlotInfo SaveStore::Describe(const std::string& path, int slot, bool autosave,
			std::string& error) const
		{
			SlotInfo info;
			info.slot = slot;
			info.autosave = autosave;
			std::vector<uint8_t> bytes;
			if(!ReadFileBytes(path, bytes, error))
				return info;
			info.bytes = bytes.size();
			SaveState state;
			if(!DeserializeState(bytes.data(), bytes.size(), state, error))
				return info;
			info.exists = true;
			info.timestampMs = state.timestampMs;
			info.displayTime = state.timestampMs ? SaveState::FormatTime(state.timestampMs)
				: std::string(SaveState::kNoData);
			info.scriptFile = state.scriptFile;
			info.startLabel = state.startLabel;
			info.pageIndex = state.pageIndex;
			info.speaker = state.speaker;
			info.text = state.text;
			info.vars = state.vars.size();
			info.layers = state.layers.size();
			info.readMarks = state.readKeys.size();
			info.backlog = state.backlog.size();

			if(!state.thumbnail.fileName.empty())
			{
				info.thumbSha1 = state.thumbnail.sha1;
				info.thumbWidth = state.thumbnail.width;
				info.thumbHeight = state.thumbnail.height;
				const std::string thumbPath = m_root + "/" + state.thumbnail.fileName;
				struct stat st;
				if(stat(thumbPath.c_str(), &st) == 0)
					info.hasThumbnail = true;
				else
					info.missingThumbnail = thumbPath;
			}
			return info;
		}

		std::vector<SlotInfo> SaveStore::List(int maxSlot) const
		{
			std::vector<SlotInfo> out;
			for(int slot = 0; slot <= maxSlot; slot++)
			{
				std::string error;
				SlotInfo info = Describe(SlotFile(slot), slot, false, error);
				if(info.exists)
					out.push_back(info);
			}
			std::string error;
			SlotInfo autosave = Describe(AutoFile(), -1, true, error);
			if(autosave.exists)
				out.push_back(autosave);
			return out;
		}

		bool SaveStore::EraseSlot(int slot, std::string& error)
		{
			if(unlink(SlotFile(slot).c_str()) != 0)
			{
				error = "cannot remove " + SlotFile(slot) + ": " + strerror(errno);
				return false;
			}
			// the thumbnail is ours to remove; a missing one is not an error
			unlink(ThumbFile(slot).c_str());
			return true;
		}

		bool SaveStore::EraseAutosave(std::string& error)
		{
			if(unlink(AutoFile().c_str()) != 0)
			{
				error = "cannot remove " + AutoFile() + ": " + strerror(errno);
				return false;
			}
			unlink(AutoThumbFile().c_str());
			return true;
		}

		// ------------------------------------------------- read marks, backlog

		bool SaveStore::LoadReadMarks(AlreadyRead& marks, std::string& error) const
		{
			struct stat st;
			if(stat(ReadMarksFile().c_str(), &st) != 0)
			{
				marks.Clear(); // no file yet is the normal first run, not a failure
				return true;
			}
			return marks.LoadFrom(ReadMarksFile(), error);
		}

		bool SaveStore::SaveReadMarks(const AlreadyRead& marks, std::string& error) const
		{
			return marks.SaveTo(ReadMarksFile(), error);
		}

		bool SaveStore::LoadBacklog(Backlog& ring, std::string& error) const
		{
			struct stat st;
			if(stat(BacklogFile().c_str(), &st) != 0)
			{
				ring.Clear();
				return true;
			}
			return ring.LoadFrom(BacklogFile(), error);
		}

		bool SaveStore::SaveBacklog(const Backlog& ring, std::string& error) const
		{
			return ring.SaveTo(BacklogFile(), error);
		}
	}
}