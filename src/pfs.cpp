// pfs.cpp - reading and writing the Artemis archive

#include "pfs.h"

#include <cstring>
#include <stdexcept>

#include <CommonCrypto/CommonDigest.h>

namespace artemis
{
	static const size_t INDEX_START = 0x07;
	static const size_t ENTRIES_START = 0x0B;
	static const size_t FILESIZE_OFFSETS_START = 0x0F;

	static uint32_t ReadU32(const std::vector<uint8_t>& d, size_t at)
	{
		if(at + 4 > d.size())
			throw std::runtime_error("pfs: index runs past the end of the file");
		return (uint32_t)d[at] | (uint32_t)d[at + 1] << 8 | (uint32_t)d[at + 2] << 16
			| (uint32_t)d[at + 3] << 24;
	}

	static void AppendU32(std::vector<uint8_t>& d, uint32_t v)
	{
		d.push_back((uint8_t)(v & 0xff));
		d.push_back((uint8_t)((v >> 8) & 0xff));
		d.push_back((uint8_t)((v >> 16) & 0xff));
		d.push_back((uint8_t)((v >> 24) & 0xff));
	}

	static void AppendU64(std::vector<uint8_t>& d, uint64_t v)
	{
		for(int i = 0; i < 8; i++)
			d.push_back((uint8_t)((v >> (i * 8)) & 0xff));
	}

	void Sha1(const uint8_t* data, size_t length, uint8_t out[20])
	{
		CC_SHA1(data, (CC_LONG)length, out);
	}

	Archive Read(const std::vector<uint8_t>& data)
	{
		if(data.size() < ENTRIES_START)
			throw std::runtime_error("pfs: too short to be an archive");
		Archive a;
		a.magic.assign((const char*)&data[0], 3);
		if(a.magic != "pf6" && a.magic != "pf8")
			throw std::runtime_error("pfs: not a pf6/pf8 archive");
		a.raw = data;

		const uint32_t indexSize = ReadU32(data, 0x03);
		const uint32_t indexCount = ReadU32(data, 0x07);
		if(indexSize < 4 || INDEX_START + indexSize > data.size())
			throw std::runtime_error("pfs: index size runs past the end of the file");

		// the content key is the SHA1 of the index bytes
		uint8_t key[20];
		Sha1(&data[INDEX_START], indexSize, key);
		a.key.assign(key, key + 20);

		size_t cursor = ENTRIES_START;
		const size_t indexEnd = INDEX_START + indexSize;
		for(uint32_t i = 0; i < indexCount; i++)
		{
			if(cursor + 4 > indexEnd)
				throw std::runtime_error("pfs: index ends inside an entry");
			Entry e;
			const uint32_t nameLength = ReadU32(data, cursor);
			cursor += 4;
			if(cursor + nameLength + 12 > indexEnd)
				throw std::runtime_error("pfs: entry runs past the index");
			e.name.assign((const char*)&data[cursor], nameLength);
			cursor += nameLength + 4; // name, then four reserved bytes
			e.offset = ReadU32(data, cursor);
			e.size = ReadU32(data, cursor + 4);
			cursor += 8;
			// the writer marks a file as encrypted by the top bit of its path
			// length in some builds; here every content is encrypted the same way
			e.encrypted = true;
			a.entries.push_back(e);
		}
		return a;
	}

	std::vector<uint8_t> Content(const Archive& archive, const Entry& entry)
	{
		if((size_t)entry.offset + entry.size > archive.raw.size())
			throw std::runtime_error("pfs: entry runs past the end of the file");
		std::vector<uint8_t> out(archive.raw.begin() + entry.offset,
			archive.raw.begin() + entry.offset + entry.size);
		if(entry.encrypted && archive.key.size() == 20)
			for(size_t i = 0; i < out.size(); i++)
				out[i] ^= archive.key[(entry.offset + i) % 20];
		return out;
	}

	std::vector<uint8_t> Build(
		const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files,
		const std::string& magic, bool encrypt)
	{
		// The index is everything from 0x07 on: the count, the entries and the
		// trailing size-field table.  Its size does not depend on the offsets, so
		// one pass over a placeholder sizes it and the second pass fills it in.
		auto buildIndex = [&](uint32_t dataStart) {
			std::vector<uint8_t> index;
			AppendU32(index, (uint32_t)files.size());
			uint32_t at = dataStart;
			for(auto& f : files)
			{
				AppendU32(index, (uint32_t)f.first.size());
				index.insert(index.end(), f.first.begin(), f.first.end());
				AppendU32(index, 0); // reserved
				AppendU32(index, at);
				AppendU32(index, (uint32_t)f.second.size());
				at += (uint32_t)f.second.size();
			}
			const uint32_t countOffset = (uint32_t)(index.size() - 4); // where the count sits, from 0x07
			AppendU32(index, (uint32_t)files.size() + 1);
			for(size_t i = 0; i < files.size(); i++)
				AppendU64(index, 0); // the size-field offsets; this reader does not use them
			AppendU64(index, 0);     // end marker
			AppendU32(index, countOffset);
			return index;
		};

		const std::vector<uint8_t> sized = buildIndex(0);
		const uint32_t indexSize = (uint32_t)sized.size();
		const uint32_t dataStart = (uint32_t)(INDEX_START + indexSize);
		const std::vector<uint8_t> index = buildIndex(dataStart);

		std::vector<uint8_t> out;
		out.insert(out.end(), magic.begin(), magic.end());
		AppendU32(out, indexSize);
		out.insert(out.end(), index.begin(), index.end());

		// the content key is the SHA1 of the index, which is now in place
		uint8_t key[20];
		Sha1(&out[INDEX_START], indexSize, key);
		for(auto& f : files)
		{
			std::vector<uint8_t> content = f.second;
			if(encrypt)
			{
				const uint32_t offset = (uint32_t)out.size();
				for(size_t i = 0; i < content.size(); i++)
					content[i] ^= key[(offset + i) % 20];
			}
			out.insert(out.end(), content.begin(), content.end());
		}
		return out;
	}
}
