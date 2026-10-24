// pfs.h - the Artemis engine's archive format, written from the format
//
// Artemis (the engine of the 2013+ titles the port targets) keeps a game in
// `<something>.pfs` archives: a header with an index of names, offsets and
// sizes, then the file contents, XOR-encrypted with a key derived from the
// index itself.  Two generations exist - "pf6" and "pf8" - with the same shape.
//
// Layout, all integers little-endian:
//
//   magic[3]        "pf6" or "pf8"
//   u32 index_size  bytes of index, counted from INDEX_START (0x07)
//   u32 index_count number of files
//   entries[]       u32 name_length, name (no terminator), 4 zero bytes,
//                   u32 offset, u32 size
//   u32 count+1
//   u64 offsets[]   position of each entry's size field, from 0x0F
//   u64 0           end marker
//   u32 count_off   position of the count above, from 0x07
//
// The content of a file is XORed with SHA1(index bytes) taken twenty bytes at a
// time, advancing with the file position.

#ifndef ARTEMIS_PFS_H
#define ARTEMIS_PFS_H

#include <cstdint>
#include <string>
#include <vector>

namespace artemis
{
	struct Entry
	{
		std::string name; // the path inside the archive
		uint32_t offset;  // where the content starts in the archive
		uint32_t size;    // its length
		bool encrypted;   // whether the writer encrypted it
	};

	struct Archive
	{
		std::string magic; // "pf6" or "pf8"
		std::vector<Entry> entries;
		std::vector<uint8_t> raw; // the whole file
		std::vector<uint8_t> key; // SHA1 of the index, 20 bytes
	};

	// Read an archive from memory; throws std::runtime_error with a reason.
	Archive Read(const std::vector<uint8_t>& data);

	// The content of one entry, decrypted when it needs to be.
	std::vector<uint8_t> Content(const Archive& archive, const Entry& entry);

	// Build an archive from name/content pairs.  `magic` selects the generation,
	// and `encrypt` the XOR layer the real archives carry.
	std::vector<uint8_t> Build(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files,
		const std::string& magic = "pf8", bool encrypt = true);

	// SHA1, in the one place it is used (the content key).
	void Sha1(const uint8_t* data, size_t length, uint8_t out[20]);
}

#endif // ARTEMIS_PFS_H

// ---------------------------------------------------------------------------
// the split spelling
// ---------------------------------------------------------------------------
//
// A shipped title keeps its data in several archives named "<game>.pfs.<NNN>",
// each an archive of its own (see docs/GAME_LAYOUT.md).  These list what a game
// has and read any one of them, so a caller can work from the game folder rather
// than from a file name it has to know in advance.

namespace artemis
{
	// the archive files in `directory` whose name starts with `stem` (for example
	// "Amakano3"), in suffix order; empty when there are none
	std::vector<std::string> FindArchives(const std::string& directory, const std::string& stem);

	// the concatenated entries of every archive found, each archive read on its own
	std::vector<Entry> ListAll(const std::vector<std::string>& paths);
}
