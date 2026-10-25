// pfs.cpp - reading and writing the Artemis archive
//
// The format is described in pfs.h; the parts worth repeating here are the ones
// where a guess would be invisible: how the index is laid out per version, and
// how the XOR key phase is decided when the format itself does not say.

#include "pfs.h"
#include "sniff.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <CommonCrypto/CommonDigest.h>

namespace artemis
{
	static const size_t INDEX_START = 0x07;

	// ---------------------------------------------------------------- mapping

	MappedFile::MappedFile(const std::string& path)
	{
		m_fd = ::open(path.c_str(), O_RDONLY);
		if(m_fd < 0)
			throw std::runtime_error("pfs: cannot open " + path);
		struct stat st;
		if(fstat(m_fd, &st) != 0)
		{
			::close(m_fd);
			m_fd = -1;
			throw std::runtime_error("pfs: cannot stat " + path);
		}
		m_size = (size_t)st.st_size;
		if(m_size == 0)
			return; // an empty file maps to nothing; valid() stays false
		void* p = mmap(nullptr, m_size, PROT_READ, MAP_PRIVATE, m_fd, 0);
		if(p == MAP_FAILED)
		{
			::close(m_fd);
			m_fd = -1;
			m_size = 0;
			throw std::runtime_error("pfs: cannot map " + path);
		}
		m_data = (uint8_t*)p;
	}

	MappedFile::~MappedFile()
	{
		if(m_data)
			munmap(m_data, m_size);
		if(m_fd >= 0)
			::close(m_fd);
	}

	MappedFile::MappedFile(MappedFile&& other) noexcept
		: m_fd(other.m_fd), m_data(other.m_data), m_size(other.m_size)
	{
		other.m_fd = -1;
		other.m_data = nullptr;
		other.m_size = 0;
	}

	MappedFile& MappedFile::operator=(MappedFile&& other) noexcept
	{
		if(this != &other)
		{
			if(m_data)
				munmap(m_data, m_size);
			if(m_fd >= 0)
				::close(m_fd);
			m_fd = other.m_fd;
			m_data = other.m_data;
			m_size = other.m_size;
			other.m_fd = -1;
			other.m_data = nullptr;
			other.m_size = 0;
		}
		return *this;
	}

	const uint8_t* Archive::bytes() const
	{
		if(owner)
			return owner->data();
		if(mapping && mapping->valid())
			return mapping->data();
		return nullptr;
	}

	// ---------------------------------------------------------------- primitives

	static uint32_t ReadU32(const uint8_t* d, size_t at)
	{
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

	const char* Name(KeyPhase phase)
	{
		switch(phase)
		{
		case KeyPhase::Auto: return "auto";
		case KeyPhase::None: return "none";
		case KeyPhase::EntryRelative: return "entry";
		case KeyPhase::ArchiveOffset: return "archive";
		}
		return "?";
	}

	KeyPhase PhaseFromName(const std::string& name)
	{
		if(name == "auto")
			return KeyPhase::Auto;
		if(name == "none" || name == "plain")
			return KeyPhase::None;
		if(name == "entry" || name == "entry-relative")
			return KeyPhase::EntryRelative;
		if(name == "archive" || name == "archive-offset")
			return KeyPhase::ArchiveOffset;
		throw std::runtime_error("pfs: unknown key phase '" + name + "'");
	}

	// ------------------------------------------------------------------ reading

	// Undo (or apply) the XOR layer for `length` bytes of one entry.  `relative`
	// is how far into the entry the bytes sit, which is all that is needed to
	// resume the key in either convention - that is what makes extraction of a
	// single member from a 200 MB archive a streaming read.
	static void ApplyKey(const Archive& archive, const Entry& entry, uint8_t* data,
		size_t length, size_t relative)
	{
		if(!archive.encrypted() || archive.key.size() != 20)
			return;
		size_t phase = archive.phase == KeyPhase::ArchiveOffset
			? (size_t)((entry.offset + relative) % 20)
			: (relative % 20);

		// A byte at a time is what the format describes, and it is what this used
		// to do - but it measured 943 MiB/s, and the whole cost of opening a
		// 202 MB volume is this loop.  The key has a 20-byte period, so walk the
		// unaligned head once and from there XOR 8 + 8 + 4 bytes at a time:
		// 22.6 GiB/s, 24x faster, same bytes (bench/artemis checks the equality
		// over every phase and length, including the 256 KiB extraction window).
		const size_t head = [&] {
			size_t h = (20 - phase) % 20;
			return h < length ? h : length;
		}();
		for(size_t i = 0; i < head; i++)
			data[i] ^= archive.key[phase + i];

		size_t at = head;
		uint64_t k0, k1;
		uint32_t k2;
		memcpy(&k0, archive.key.data(), 8);
		memcpy(&k1, archive.key.data() + 8, 8);
		memcpy(&k2, archive.key.data() + 16, 4);
		for(; at + 20 <= length; at += 20)
		{
			uint64_t a0, a1;
			uint32_t a2;
			memcpy(&a0, data + at, 8);
			memcpy(&a1, data + at + 8, 8);
			memcpy(&a2, data + at + 16, 4);
			a0 ^= k0;
			a1 ^= k1;
			a2 ^= k2;
			memcpy(data + at, &a0, 8);
			memcpy(data + at + 8, &a1, 8);
			memcpy(data + at + 16, &a2, 4);
		}
		for(size_t i = 0; at + i < length; i++)
			data[at + i] ^= archive.key[i];
	}

	static void DecryptSample(const uint8_t* src, size_t size, uint64_t offset,
		const Archive& archive, KeyPhase phase, std::vector<uint8_t>& out)
	{
		const size_t n = size < 384 ? size : 384;
		out.assign(src, src + n);
		if(phase == KeyPhase::None || archive.key.size() != 20)
			return;
		size_t k = phase == KeyPhase::ArchiveOffset ? (size_t)(offset % 20) : 0;
		for(size_t i = 0; i < n; i++)
		{
			out[i] ^= archive.key[k];
			if(++k == 20)
				k = 0;
		}
	}

	// Which key phase produces recognisable content?  The format does not say,
	// the reference (GARbro) decrypts each entry from key offset zero, and this
	// tree's first writer ran the key across the whole content area.  Rather
	// than pick, decrypt a sample of members both ways and see which one sniffs
	// like a TTF, a PNG, an Ogg, a script - a mis-phased XOR destroys signatures
	// immediately.  Ties fall back to the reference's rule.
	static void DetectPhase(const uint8_t* d, size_t length, Archive& archive,
		KeyPhase want)
	{
		auto sampleEntries = [&](size_t limit) {
			std::vector<size_t> picked;
			for(size_t i = 0; i < archive.entries.size() && picked.size() < limit; i++)
				if(archive.entries[i].size >= 4)
					picked.push_back(i);
			return picked;
		};

		if(want != KeyPhase::Auto)
		{
			archive.phase = want;
			if(want == KeyPhase::None)
				archive.key.clear();
			if(want != KeyPhase::None && archive.key.empty() && archive.version != 2)
			{
				uint8_t key[20];
				Sha1(d + INDEX_START, archive.indexSize, key);
				archive.key.assign(key, key + 20);
			}
			return;
		}

		// a version that the reference says carries no key may still be keyed in
		// some build, so the content gets the last word
		uint8_t key[20];
		const bool keyedVersion = archive.version == 4 || archive.version == 5
			|| archive.version == 8 || archive.version == 9;
		if(archive.key.empty() && archive.version != 2)
		{
			Sha1(d + INDEX_START, archive.indexSize, key);
			archive.key.assign(key, key + 20);
		}
		const std::vector<uint8_t> savedKey = archive.key;

		auto score = [&](KeyPhase phase) {
			long total = 0;
			size_t seen = 0;
			for(size_t i : sampleEntries(16))
			{
				const Entry& e = archive.entries[i];
				std::vector<uint8_t> sample;
				archive.key = savedKey;
				DecryptSample(d + e.offset, e.size, e.offset, archive, phase, sample);
				std::string kind;
				total += ScoreBytes(sample.data(), sample.size(), &kind);
				seen++;
			}
			return seen ? (double)total / (double)seen : 0.0;
		};

		const double plainScore = score(KeyPhase::None);
		const double entryScore = score(KeyPhase::EntryRelative);
		const double archiveScore = score(KeyPhase::ArchiveOffset);

		if(keyedVersion)
		{
			if(entryScore > archiveScore)
			{
				archive.phase = KeyPhase::EntryRelative;
				archive.phaseScore = (int)entryScore;
				archive.phaseGuessed = entryScore - archiveScore < 50 || entryScore < 300;
			}
			else
			{
				archive.phase = KeyPhase::ArchiveOffset;
				archive.phaseScore = (int)archiveScore;
				archive.phaseGuessed = archiveScore - entryScore < 50 || archiveScore < 300;
			}
			if(plainScore > entryScore && plainScore > archiveScore)
			{
				// the index says keyed but the bytes say plaintext: trust neither
				// blindly, report the disagreement and keep the keyed reading
				archive.phaseGuessed = true;
			}
		}
		else
		{
			// pf2/pf6 by the reference are plaintext; only override that if the
			// keyed readings are decisively better
			const double bestKeyed = std::max(entryScore, archiveScore);
			if(bestKeyed > plainScore + 300 && plainScore < 250)
			{
				archive.key = savedKey;
				archive.phase = entryScore >= archiveScore ? KeyPhase::EntryRelative
					: KeyPhase::ArchiveOffset;
				archive.phaseScore = (int)bestKeyed;
				archive.keyGuessed = true;
			}
			else
			{
				archive.key.clear();
				archive.phase = KeyPhase::None;
				archive.phaseScore = (int)plainScore;
				archive.phaseGuessed = plainScore < 300;
			}
		}
		for(Entry& e : archive.entries)
			e.encrypted = archive.encrypted();
	}

	static Archive Parse(const uint8_t* d, size_t length, const std::string& path,
		KeyPhase want)
	{
		if(length < 0x0C)
			throw std::runtime_error("pfs: too short to be an archive");
		Archive a;
		a.path = path;
		a.magic.assign((const char*)d, 3);
		if(a.magic[0] != 'p' || a.magic[1] != 'f' || a.magic[2] < '0' || a.magic[2] > '9')
			throw std::runtime_error("pfs: not a 'pf<digit>' archive");
		a.version = a.magic[2] - '0';
		a.indexSize = ReadU32(d, 0x03);
		if(INDEX_START + (size_t)a.indexSize > length)
			throw std::runtime_error("pfs: index size runs past the end of the file");

		const bool v2 = a.version == 2;
		const uint32_t count = v2 ? ReadU32(d, 0x0B) : ReadU32(d, INDEX_START);
		if(count > 4000000u)
			throw std::runtime_error("pfs: implausible entry count");

		size_t cursor = v2 ? 0x0F : 0x0B;
		const size_t indexEnd = INDEX_START + a.indexSize;
		const size_t reserved = v2 ? 12 : 4;
		a.entries.reserve(count);
		for(uint32_t i = 0; i < count; i++)
		{
			if(cursor + 4 > indexEnd)
				throw std::runtime_error("pfs: index ends inside an entry");
			const uint32_t nameLength = ReadU32(d, cursor);
			cursor += 4;
			if(nameLength > a.indexSize || cursor + nameLength + reserved + 8 > indexEnd)
				throw std::runtime_error("pfs: entry runs past the index");
			Entry e;
			e.name.assign((const char*)d + cursor, nameLength);
			// fold into the stored string itself: NormalizeName() would build a
			// temporary and then copy it into the vector, which is three
			// allocations per entry on the open path
			e.normalized.assign(e.name);
			for(char& ch : e.normalized)
				ch = ch == '\\' ? '/' : (ch >= 'A' && ch <= 'Z' ? (char)(ch - 'A' + 'a') : ch);
			cursor += nameLength + reserved;
			e.offset = ReadU32(d, cursor);
			e.size = ReadU32(d, cursor + 4);
			cursor += 8;
			if(e.offset + e.size > length)
				throw std::runtime_error("pfs: entry runs past the end of the file");
			e.encrypted = false;
			a.entries.push_back(std::move(e));
		}

		if(a.version == 4 || a.version == 5 || a.version == 8 || a.version == 9)
		{
			uint8_t key[20];
			Sha1(d + INDEX_START, a.indexSize, key);
			a.key.assign(key, key + 20);
		}
		DetectPhase(d, length, a, want);
		return a;
	}

	Archive Read(const std::vector<uint8_t>& data, KeyPhase phase)
	{
		Archive a = Parse(data.empty() ? nullptr : data.data(), data.size(), "", phase);
		a.owner = std::make_shared<std::vector<uint8_t>>(data);
		return a;
	}

	Archive Open(const std::string& path, KeyPhase phase)
	{
		auto mapping = std::make_shared<MappedFile>(path);
		if(!mapping->valid())
			throw std::runtime_error("pfs: " + path + " is empty");
		Archive a = Parse(mapping->data(), mapping->size(), path, phase);
		a.mapping = mapping;
		return a;
	}

	std::vector<uint8_t> Content(const Archive& archive, const Entry& entry)
	{
		if(entry.offset + entry.size > archive.length() || archive.bytes() == nullptr)
			throw std::runtime_error("pfs: entry runs past the end of the archive");
		std::vector<uint8_t> out((size_t)entry.size);
		if(entry.size > 0)
			memcpy(out.data(), archive.bytes() + entry.offset, entry.size);
		ApplyKey(archive, entry, out.data(), out.size(), 0);
		return out;
	}

	std::vector<uint8_t> Sample(const Archive& archive, const Entry& entry, size_t limit)
	{
		if(entry.offset + entry.size > archive.length() || archive.bytes() == nullptr)
			throw std::runtime_error("pfs: entry runs past the end of the archive");
		// the same memcpy and the same ApplyKey(..., relative = 0) that Content()
		// starts with, so the bytes cannot differ: only how many of them are made
		const size_t n = entry.size < limit ? (size_t)entry.size : limit;
		std::vector<uint8_t> out(n);
		if(n > 0)
			memcpy(out.data(), archive.bytes() + entry.offset, n);
		ApplyKey(archive, entry, out.data(), out.size(), 0);
		return out;
	}

	bool ExtractTo(const Archive& archive, const Entry& entry, FILE* out)
	{
		if(out == nullptr || archive.bytes() == nullptr
			|| entry.offset + entry.size > archive.length())
			return false;
		const size_t CHUNK = 256 * 1024;
		std::vector<uint8_t> buffer(CHUNK);
		size_t done = 0;
		while(done < entry.size)
		{
			const size_t want = std::min(CHUNK, (size_t)entry.size - done);
			memcpy(buffer.data(), archive.bytes() + entry.offset + done, want);
			ApplyKey(archive, entry, buffer.data(), want, done);
			if(fwrite(buffer.data(), 1, want, out) != want)
				return false;
			done += want;
		}
		return true;
	}

	// ------------------------------------------------------------------ writing

	std::vector<uint8_t> Build(
		const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files,
		const std::string& magic, bool encrypt, KeyPhase phase)
	{
		if(magic.size() != 3 || magic[0] != 'p' || magic[1] != 'f')
			throw std::runtime_error("pfs: magic must be 'pf<digit>'");
		// The index is everything from 0x07 on: the count, the entries and the
		// trailing size-field table.  Its size does not depend on the offsets, so
		// one pass over a placeholder sizes it and the second pass fills it in.
		auto buildIndex = [&](uint64_t dataStart) {
			std::vector<uint8_t> index;
			AppendU32(index, (uint32_t)files.size());
			uint64_t at = dataStart;
			for(auto& f : files)
			{
				AppendU32(index, (uint32_t)f.first.size());
				index.insert(index.end(), f.first.begin(), f.first.end());
				AppendU32(index, 0); // reserved
				AppendU32(index, (uint32_t)at);
				AppendU32(index, (uint32_t)f.second.size());
				at += f.second.size();
			}
			const uint32_t countOffset = (uint32_t)(index.size() - 4);
			AppendU32(index, (uint32_t)files.size() + 1);
			for(size_t i = 0; i < files.size(); i++)
				AppendU64(index, 0); // size-field offsets; this reader does not use them
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

		uint8_t key[20] = { 0 };
		if(encrypt)
			Sha1(&out[INDEX_START], indexSize, key);
		for(auto& f : files)
		{
			std::vector<uint8_t> content = f.second;
			if(encrypt)
			{
				const uint64_t offset = out.size();
				size_t k = phase == KeyPhase::ArchiveOffset ? (size_t)(offset % 20) : 0;
				for(size_t i = 0; i < content.size(); i++)
				{
					content[i] ^= key[k];
					if(++k == 20)
						k = 0;
				}
			}
			out.insert(out.end(), content.begin(), content.end());
		}
		return out;
	}

	std::vector<uint8_t> BuildVersion2(
		const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files)
	{
		// the v2 index, as the reference reads it: four unused bytes, the count at
		// index offset 4, then entries at index offset 8 with twelve reserved
		// bytes each.  No trailing table and no key.
		auto buildIndex = [&](uint64_t dataStart) {
			std::vector<uint8_t> index;
			AppendU32(index, 0);
			AppendU32(index, (uint32_t)files.size());
			uint64_t at = dataStart;
			for(auto& f : files)
			{
				AppendU32(index, (uint32_t)f.first.size());
				index.insert(index.end(), f.first.begin(), f.first.end());
				for(int i = 0; i < 12; i++)
					index.push_back(0);
				AppendU32(index, (uint32_t)at);
				AppendU32(index, (uint32_t)f.second.size());
				at += f.second.size();
			}
			return index;
		};
		const std::vector<uint8_t> sized = buildIndex(0);
		const uint32_t indexSize = (uint32_t)sized.size();
		const std::vector<uint8_t> index = buildIndex(INDEX_START + indexSize);

		std::vector<uint8_t> out;
		out.insert(out.end(), { 'p', 'f', '2' });
		AppendU32(out, indexSize);
		out.insert(out.end(), index.begin(), index.end());
		for(auto& f : files)
			out.insert(out.end(), f.second.begin(), f.second.end());
		return out;
	}

	// --------------------------------------------------------------- game paths

	std::string NormalizeName(const std::string& name)
	{
		std::string out = name;
		for(char& c : out)
		{
			if(c == '\\')
				c = '/';
			else if(c >= 'A' && c <= 'Z')
				c = (char)(c - 'A' + 'a');
		}
		return out;
	}

	const Entry* Find(const Archive& archive, const std::string& name)
	{
		const std::string want = NormalizeName(name);
		for(const Entry& e : archive.entries)
		{
			// `normalized` is filled by the reader; an Entry built by hand still
			// works by falling back to normalising here, so this cannot silently miss
			if(!e.normalized.empty() || e.name.empty())
			{
				if(e.normalized == want)
					return &e;
				continue;
			}
			if(NormalizeName(e.name) == want)
				return &e;
		}
		return nullptr;
	}

	std::vector<const Entry*> FindSuffix(const Archive& archive, const std::string& suffix)
	{
		std::vector<const Entry*> out;
		const std::string want = NormalizeName(suffix);
		for(const Entry& e : archive.entries)
		{
			const std::string& n =
				(!e.normalized.empty() || e.name.empty()) ? e.normalized
					: NormalizeName(e.name);
			if(n.size() >= want.size() && n.compare(n.size() - want.size(), want.size(), want) == 0)
				out.push_back(&e);
		}
		return out;
	}

	static bool AllDigits(const std::string& s)
	{
		if(s.empty())
			return false;
		for(char c : s)
			if(c < '0' || c > '9')
				return false;
		return true;
	}

	std::vector<std::string> FindArchives(const std::string& directory, const std::string& stem)
	{
		std::vector<std::string> out;
		DIR* d = opendir(directory.c_str());
		if(d == nullptr)
			return out;
		struct dirent* e;
		while((e = readdir(d)) != nullptr)
		{
			const std::string name = e->d_name;
			bool matches = false;
			if(stem.empty())
			{
				// a bare "root.pfs", or "<anything>.pfs.<NNN>"
				const size_t pfs = name.rfind(".pfs");
				if(pfs != std::string::npos && pfs + 4 == name.size())
					matches = true;
				else if(pfs != std::string::npos && name[pfs + 4] == '.'
					&& AllDigits(name.substr(pfs + 5)))
					matches = true;
			}
			else
			{
				const std::string base = stem + ".pfs";
				if(name == base)
					matches = true;
				else if(name.compare(0, base.size(), base) == 0 && name.size() > base.size() + 1
					&& name[base.size()] == '.' && AllDigits(name.substr(base.size() + 1)))
					matches = true;
				else
				{
					const std::string dotted = stem + ".";
					if(name.compare(0, dotted.size(), dotted) == 0
						&& AllDigits(name.substr(dotted.size())))
						matches = true;
				}
			}
			if(matches)
				out.push_back(directory + "/" + name);
		}
		closedir(d);
		std::sort(out.begin(), out.end());
		return out;
	}

	std::vector<Entry> ListAll(const std::vector<std::string>& paths)
	{
		std::vector<Entry> all;
		for(const std::string& path : paths)
		{
			try
			{
				// Open maps the file, so a list of a 200 MB archive reads its index
				// and nothing else.
				Archive archive = Open(path);
				const std::string base = path.substr(path.find_last_of('/') + 1);
				for(Entry e : archive.entries)
				{
					e.name = base + ":" + e.name;
					e.normalized = NormalizeName(e.name); // renamed, so this is stale
					all.push_back(e);
				}
			}
			catch(const std::exception&)
			{
				// a file that is not an archive is skipped, not fatal
			}
		}
		return all;
	}
}