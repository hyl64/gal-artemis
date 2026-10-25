// format.cpp - the save file's bytes, as described in format.h

#include "format.h"

#include <cstdio>
#include <cstring>

#include <CommonCrypto/CommonDigest.h>

namespace artemis
{
	namespace save
	{
		namespace
		{
			// ------------------------------------------------------- writing

			void PutU32(std::vector<uint8_t>& out, uint32_t v)
			{
				for(int i = 0; i < 4; i++)
					out.push_back((uint8_t)((v >> (i * 8)) & 0xff));
			}

			void PutI32(std::vector<uint8_t>& out, int32_t v)
			{
				PutU32(out, (uint32_t)v);
			}

			void PutU64(std::vector<uint8_t>& out, uint64_t v)
			{
				for(int i = 0; i < 8; i++)
					out.push_back((uint8_t)((v >> (i * 8)) & 0xff));
			}

			void PutI64(std::vector<uint8_t>& out, int64_t v)
			{
				PutU64(out, (uint64_t)v);
			}

			void PutStr(std::vector<uint8_t>& out, const std::string& s)
			{
				PutU32(out, (uint32_t)s.size());
				out.insert(out.end(), s.begin(), s.end());
			}

			void PutBytes(std::vector<uint8_t>& out, const uint8_t* data, size_t n)
			{
				out.insert(out.end(), data, data + n);
			}

			// ------------------------------------------------------- reading

			struct Reader
			{
				const uint8_t* data;
				size_t size;
				size_t at = 0;
				std::string why;

				bool Need(size_t n)
				{
					if(at + n > size)
					{
						why = "the file ends inside section data at offset "
							+ std::to_string(at);
						return false;
					}
					return true;
				}

				bool U32(uint32_t& v)
				{
					if(!Need(4))
						return false;
					v = (uint32_t)data[at] | (uint32_t)data[at + 1] << 8
						| (uint32_t)data[at + 2] << 16 | (uint32_t)data[at + 3] << 24;
					at += 4;
					return true;
				}

				bool I32(int32_t& v)
				{
					uint32_t raw = 0;
					if(!U32(raw))
						return false;
					v = (int32_t)raw;
					return true;
				}

				bool U64(uint64_t& v)
				{
					if(!Need(8))
						return false;
					v = 0;
					for(int i = 0; i < 8; i++)
						v |= (uint64_t)data[at + i] << (i * 8);
					at += 8;
					return true;
				}

				bool I64(int64_t& v)
				{
					uint64_t raw = 0;
					if(!U64(raw))
						return false;
					v = (int64_t)raw;
					return true;
				}

				bool Str(std::string& s)
				{
					uint32_t n = 0;
					if(!U32(n))
						return false;
					if(!Need(n))
						return false;
					s.assign((const char*)data + at, n);
					at += n;
					return true;
				}

				bool U8(uint8_t& v)
				{
					if(!Need(1))
						return false;
					v = data[at++];
					return true;
				}

				bool F64(double& v)
				{
					uint64_t raw = 0;
					if(!U64(raw))
						return false;
					memcpy(&v, &raw, 8);
					return true;
				}
			};

			// one section, in and out
			void WriteSection(std::vector<uint8_t>& out, const char* tag,
				const std::vector<uint8_t>& body)
			{
				out.insert(out.end(), tag, tag + 4);
				PutU32(out, (uint32_t)body.size());
				out.insert(out.end(), body.begin(), body.end());
			}

			std::string Tag(const uint8_t* p)
			{
				return std::string((const char*)p, 4);
			}
		}

		uint32_t Crc32(const uint8_t* data, size_t size)
		{
			uint32_t crc = 0xFFFFFFFFu;
			for(size_t i = 0; i < size; i++)
			{
				crc ^= data[i];
				for(int bit = 0; bit < 8; bit++)
					crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
			}
			return ~crc;
		}

		std::string Sha1Hex(const uint8_t* data, size_t size)
		{
			uint8_t digest[20];
			CC_SHA1(data, (CC_LONG)size, digest);
			static const char* hex = "0123456789abcdef";
			std::string out;
			out.reserve(40);
			for(int i = 0; i < 20; i++)
			{
				out.push_back(hex[digest[i] >> 4]);
				out.push_back(hex[digest[i] & 0xf]);
			}
			return out;
		}

		std::vector<uint8_t> SerializeState(const SaveState& state)
		{
			std::vector<uint8_t> payload;

			{
				std::vector<uint8_t> body;
				PutU32(body, state.formatVersion);
				PutI32(body, state.slot);
				body.push_back(state.autosave ? 1 : 0);
				PutI64(body, state.timestampMs);
				PutStr(body, state.displayTime);
				PutStr(body, state.gameId);
				PutStr(body, state.engineVersion);
				WriteSection(payload, "META", body);
			}
			{
				std::vector<uint8_t> body;
				PutStr(body, state.scriptFile);
				PutStr(body, state.startLabel);
				PutStr(body, state.block);
				PutU32(body, (uint32_t)state.pageIndex);
				PutStr(body, state.speaker);
				PutStr(body, state.text);
				WriteSection(payload, "POS ", body);
			}
			{
				std::vector<uint8_t> body;
				PutU32(body, (uint32_t)state.vars.size());
				for(const auto& v : state.vars)
				{
					PutStr(body, v.first);
					PutStr(body, v.second);
				}
				WriteSection(payload, "VARS", body);
			}
			{
				std::vector<uint8_t> body;
				PutU32(body, (uint32_t)state.layers.size());
				for(const LayerState& layer : state.layers)
				{
					PutStr(body, layer.id);
					PutStr(body, layer.file);
					body.push_back(layer.visible ? 1 : 0);
					PutI32(body, layer.left);
					PutI32(body, layer.top);
					PutI32(body, layer.width);
					PutI32(body, layer.height);
					PutI32(body, layer.xscale);
					PutI32(body, layer.yscale);
					PutI32(body, layer.alpha);
					PutBytes(body, layer.color, 4);
					PutStr(body, layer.eventFile);
					PutStr(body, layer.eventLabel);
					PutI32(body, layer.slot);
				}
				WriteSection(payload, "LAYR", body);
			}
			{
				std::vector<uint8_t> body;
				PutStr(body, state.bgm.file);
				PutStr(body, state.bgm.path);
				body.push_back(state.bgm.playing ? 1 : 0);
				PutI32(body, state.bgm.positionMs);
				PutI32(body, state.bgm.loop);
				// the volume is a double, written as its own bit pattern
				uint64_t bits = 0;
				memcpy(&bits, &state.bgm.volume, 8);
				PutU64(body, bits);
				WriteSection(payload, "BGM ", body);
			}
			{
				std::vector<uint8_t> body;
				PutU32(body, (uint32_t)state.readKeys.size());
				for(const std::string& key : state.readKeys)
					PutStr(body, key);
				WriteSection(payload, "READ", body);
			}
			{
				std::vector<uint8_t> body;
				PutU32(body, (uint32_t)state.backlog.size());
				for(const BacklogEntry& entry : state.backlog)
				{
					PutStr(body, entry.speaker);
					PutStr(body, entry.text);
					PutStr(body, entry.file);
					PutI32(body, entry.line);
					PutI64(body, entry.atMs);
				}
				WriteSection(payload, "BLCK", body);
			}
			{
				std::vector<uint8_t> body;
				PutStr(body, state.thumbnail.fileName);
				PutI32(body, state.thumbnail.width);
				PutI32(body, state.thumbnail.height);
				PutStr(body, state.thumbnail.sha1);
				WriteSection(payload, "THUM", body);
			}

			std::vector<uint8_t> out;
			out.insert(out.end(), kMagic, kMagic + 8);
			PutU32(out, kVersion);
			PutU32(out, (uint32_t)payload.size());
			PutU32(out, Crc32(payload.data(), payload.size()));
			out.insert(out.end(), payload.begin(), payload.end());
			return out;
		}

		bool DeserializeState(const uint8_t* data, size_t size, SaveState& out,
			std::string& error)
		{
			if(size < 8 || memcmp(data, kMagic, 8) != 0)
			{
				error = "not an Artemis save file (the magic is not \"ARTSAVE1\")";
				return false;
			}
			Reader reader{ data, size };
			reader.at = 8;
			uint32_t version = 0, payloadLength = 0, crc = 0;
			if(!reader.U32(version) || !reader.U32(payloadLength) || !reader.U32(crc))
			{
				error = "the save header is cut short";
				return false;
			}
			if(version == 0 || version > kVersion)
			{
				error = "unsupported save version " + std::to_string(version)
					+ " (this build reads up to " + std::to_string(kVersion) + ")";
				return false;
			}
			if(!reader.Need(payloadLength))
			{
				error = "the save payload is shorter than its header says";
				return false;
			}
			const uint8_t* payload = data + reader.at;
			const uint32_t actual = Crc32(payload, payloadLength);
			if(actual != crc)
			{
				char buffer[96];
				snprintf(buffer, sizeof(buffer),
					"the save payload checksum is wrong (stored %08x, computed %08x)", crc,
					actual);
				error = buffer;
				return false;
			}

			out = SaveState{};
			Reader section{ payload, payloadLength };
			while(section.at < section.size)
			{
				if(!section.Need(8))
				{
					error = "a save section header is cut short";
					return false;
				}
				const std::string tag = Tag(section.data + section.at);
				section.at += 4;
				uint32_t length = 0;
				if(!section.U32(length) || !section.Need(length))
				{
					error = "section " + tag + " is cut short";
					return false;
				}
				Reader body{ section.data + section.at, length };
				section.at += length;
				bool ok = true;
				if(tag == "META")
				{
					uint32_t format = 0;
					uint8_t autoFlag = 0;
					ok = body.U32(format) && body.I32(out.slot) && body.U8(autoFlag)
						&& body.I64(out.timestampMs) && body.Str(out.displayTime)
						&& body.Str(out.gameId) && body.Str(out.engineVersion);
					out.formatVersion = format;
					out.autosave = autoFlag != 0;
				}
				else if(tag == "POS ")
				{
					uint32_t page = 0;
					ok = body.Str(out.scriptFile) && body.Str(out.startLabel)
						&& body.Str(out.block) && body.U32(page) && body.Str(out.speaker)
						&& body.Str(out.text);
					out.pageIndex = (int)page;
				}
				else if(tag == "VARS")
				{
					uint32_t count = 0;
					ok = body.U32(count) && count <= 2000000u;
					for(uint32_t i = 0; ok && i < count; i++)
					{
						std::string name, value;
						ok = body.Str(name) && body.Str(value);
						if(ok)
							out.vars[name] = value;
					}
				}
				else if(tag == "LAYR")
				{
					uint32_t count = 0;
					ok = body.U32(count) && count <= 100000u;
					for(uint32_t i = 0; ok && i < count; i++)
					{
						LayerState layer;
						uint8_t visible = 0;
						ok = body.Str(layer.id) && body.Str(layer.file) && body.U8(visible)
							&& body.I32(layer.left) && body.I32(layer.top)
							&& body.I32(layer.width) && body.I32(layer.height)
							&& body.I32(layer.xscale) && body.I32(layer.yscale)
							&& body.I32(layer.alpha);
						if(ok)
						{
							ok = body.Need(4);
							if(ok)
							{
								memcpy(layer.color, body.data + body.at, 4);
								body.at += 4;
								ok = body.Str(layer.eventFile) && body.Str(layer.eventLabel)
									&& body.I32(layer.slot);
							}
						}
						layer.visible = visible != 0;
						if(ok)
							out.layers.push_back(layer);
					}
				}
				else if(tag == "BGM ")
				{
					uint8_t playing = 0;
					ok = body.Str(out.bgm.file) && body.Str(out.bgm.path) && body.U8(playing)
						&& body.I32(out.bgm.positionMs) && body.I32(out.bgm.loop)
						&& body.F64(out.bgm.volume);
					out.bgm.playing = playing != 0;
				}
				else if(tag == "READ")
				{
					uint32_t count = 0;
					ok = body.U32(count) && count <= 2000000u;
					for(uint32_t i = 0; ok && i < count; i++)
					{
						std::string key;
						ok = body.Str(key);
						if(ok)
							out.readKeys.push_back(key);
					}
				}
				else if(tag == "BLCK")
				{
					uint32_t count = 0;
					ok = body.U32(count) && count <= 100000u;
					for(uint32_t i = 0; ok && i < count; i++)
					{
						BacklogEntry entry;
						ok = body.Str(entry.speaker) && body.Str(entry.text)
							&& body.Str(entry.file) && body.I32(entry.line)
							&& body.I64(entry.atMs);
						if(ok)
							out.backlog.push_back(entry);
					}
				}
				else if(tag == "THUM")
				{
					ok = body.Str(out.thumbnail.fileName) && body.I32(out.thumbnail.width)
						&& body.I32(out.thumbnail.height) && body.Str(out.thumbnail.sha1);
				}
				else
				{
					// a section this build does not know: the read succeeds, the
					// bytes are simply not modelled rather than misread
					continue;
				}
				if(!ok)
				{
					error = "section " + tag + " holds malformed data: " + body.why;
					return false;
				}
			}
			return true;
		}

		std::string DescribeSave(const SaveState& state)
		{
			std::string out;
			out += "magic       ARTSAVE1, format version "
				+ std::to_string(state.formatVersion) + "\n";
			out += "slot        " + std::string(state.autosave ? "autosave" :
				std::to_string(state.slot)) + "\n";
			out += "saved       " + (state.timestampMs ? SaveState::FormatTime(state.timestampMs)
				: std::string(SaveState::kNoData)) + " ("
				+ std::to_string(state.timestampMs) + " ms)\n";
			out += "story       " + (state.scriptFile.empty() ? "(none)" : state.scriptFile)
				+ (state.startLabel.empty() ? "" : (" *" + state.startLabel))
				+ (state.block.empty() ? "" : (" in *" + state.block)) + "\n";
			out += "page index  " + std::to_string(state.pageIndex) + "\n";
			out += "on screen   " + (state.speaker.empty() ? "(narration)" : ("[" + state.speaker + "]"))
				+ " " + state.text + "\n";
			out += "variables   " + std::to_string(state.vars.size()) + "\n";
			for(const auto& v : state.vars)
				out += "  " + v.first + " = " + v.second + "\n";
			out += "layers      " + std::to_string(state.layers.size()) + "\n";
			for(const LayerState& layer : state.layers)
			{
				out += "  " + layer.id + " " + (layer.visible ? "visible" : "hidden")
					+ " at " + std::to_string(layer.left) + "," + std::to_string(layer.top)
					+ " scale " + std::to_string(layer.xscale) + "%/" + std::to_string(layer.yscale)
					+ "% alpha " + std::to_string(layer.alpha);
				if(!layer.file.empty())
					out += " file " + layer.file;
				if(layer.slot >= 0)
					out += " slot " + std::to_string(layer.slot);
				out += "\n";
			}
			out += "bgm         " + (state.bgm.file.empty() ? "(none)" : state.bgm.file)
				+ (state.bgm.playing ? " playing" : " stopped")
				+ " at " + std::to_string(state.bgm.positionMs) + " ms loop "
				+ std::to_string(state.bgm.loop) + "\n";
			out += "read marks  " + std::to_string(state.readKeys.size()) + "\n";
			out += "backlog     " + std::to_string(state.backlog.size()) + " entr(y/ies)\n";
			out += "thumbnail   " + (state.thumbnail.fileName.empty() ? "(none)"
				: state.thumbnail.fileName + " " + std::to_string(state.thumbnail.width)
					+ "x" + std::to_string(state.thumbnail.height) + " sha1 "
					+ state.thumbnail.sha1) + "\n";
			return out;
		}
	}
}