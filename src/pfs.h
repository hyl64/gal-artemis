// pfs.h - the Artemis engine's archive format, written from the format
//
// Artemis (the engine of the 2013+ titles the port targets) keeps a game in
// `<something>.pfs` archives: a header with an index of names, offsets and
// sizes, then the file contents, XOR-encrypted with a key derived from the
// index itself.  Several generations exist; the reference implementation
// (GARbro's ArcFormats/Artemis/ArcPFS.cs, MIT) knows three shapes:
//
//   pf2  index_size at 0x03, count at 0x0B, entries at 0x0F, 12 reserved
//        bytes per entry, cp932 names, no encryption
//   pf4  ] count at 0x07, entries at 0x0B, 4 reserved bytes per entry
//   pf5  ] content key = SHA1(index)
//   pf6  same layout as pf8, but *no* encryption
//   pf8  same layout, content key = SHA1(index)
//   pf9  same layout as pf8 (keyed)
//
// Layout of the pf4/pf5/pf6/pf8/pf9 index, all integers little-endian:
//
//   magic[3]        "pf" + one digit
//   u32 index_size  bytes of index, counted from INDEX_START (0x07)
//   entries[]       u32 name_length, name (no terminator), 4 zero bytes,
//                   u32 offset, u32 size
//   u32 count+1
//   u64 offsets[]   position of each entry's size field, from 0x0F
//   u64 0           end marker
//   u32 count_off   position of the count above, from 0x07
//
// Real archives (verified on a shipped pf6 and a shipped pf8, see
// docs/REALDATA.md) use backslashes in member names and may be a bare
// `root.pfs` rather than `<title>.pfs.00N`.
//
// The content of a file is XORed with SHA1(index bytes) taken twenty bytes at a
// time.  Where the key phase starts is *not* something the format states, and
// the two plausible rules disagree for every file whose offset is not a
// multiple of 20:
//
//   entry    each file starts at key offset 0 (GARbro's reading)
//   archive  the key keeps running across the whole content area, so a file
//            starts at key offset `entry.offset % 20` (this tree's first writer)
//
// This reader can be told which to use and, by default, decides per archive by
// looking at which rule produces recognisable content (TTF, PNG, OggS, clean
// text).  `pfstool info` prints the decision, the score behind it and the
// sniffed kind of a few members, so real data settles the question instead of a
// guess.  GARbro's `ByteStringEncryptedStream` runs the key from the start of
// each entry stream, which is the default this reader falls back to.

#ifndef ARTEMIS_PFS_H
#define ARTEMIS_PFS_H

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace artemis
{
	struct Entry
	{
		std::string name; // the path inside the archive
		uint64_t offset;  // where the content starts in the archive
		uint32_t size;    // its length
		bool encrypted;   // whether the writer encrypted it

		// NormalizeName(name), computed once when the index is parsed.  Find() used
		// to call NormalizeName() on every candidate it looked at, which allocates a
		// string: a miss walked all 1682 names of a shipped volume and cost 71.5 us,
		// against 0.3 us for a hit that stopped early.  Loading a game misses often
		// ("is there a variant of this sprite?") so the miss is the number that
		// matters.  Appended last, so `Entry{a, b, c, d}` still compiles.
		std::string normalized;
	};

	// which key byte a file's first byte is XORed with
	enum class KeyPhase
	{
		Auto,          // decide from the content (the default)
		None,          // treat the content area as plaintext
		EntryRelative, // key offset 0 at the start of each file (GARbro)
		ArchiveOffset, // key offset `entry.offset % 20` (stream-wise)
	};

	const char* Name(KeyPhase phase);
	KeyPhase PhaseFromName(const std::string& name); // throws on a bad name

	// A read-only memory map of one file: listing a multi-hundred-megabyte
	// archive must not read it, and extracting one member must not either.
	class MappedFile
	{
	public:
		MappedFile() = default;
		explicit MappedFile(const std::string& path);
		~MappedFile();
		MappedFile(MappedFile&& other) noexcept;
		MappedFile& operator=(MappedFile&& other) noexcept;
		MappedFile(const MappedFile&) = delete;
		MappedFile& operator=(const MappedFile&) = delete;

		const uint8_t* data() const { return m_data; }
		size_t size() const { return m_size; }
		bool valid() const { return m_data != nullptr; }

	private:
		int m_fd = -1;
		uint8_t* m_data = nullptr;
		size_t m_size = 0;
	};

	struct Archive
	{
		std::string path;    // the file it came from, empty when memory-backed
		std::string magic;   // "pf2", "pf6", "pf8", ...
		int version = 0;     // the digit after "pf"
		uint32_t indexSize = 0;
		std::vector<Entry> entries;
		std::vector<uint8_t> key; // SHA1 of the index, 20 bytes; empty = plaintext
		KeyPhase phase = KeyPhase::None;
		int phaseScore = 0;        // how sure the detection was, for `info`
		bool phaseGuessed = false; // the content gave no clear answer
		bool keyGuessed = false;   // the version said plaintext, the content did not

		// backing store: either a vector we own or a mapping
		std::shared_ptr<std::vector<uint8_t>> owner;
		std::shared_ptr<MappedFile> mapping;

		const uint8_t* bytes() const;
		size_t length() const { return owner ? owner->size() : (mapping ? mapping->size() : 0); }
		bool encrypted() const { return !key.empty(); }
		bool streaming() const { return mapping && mapping->valid(); }
	};

	// Read an archive from memory; throws std::runtime_error with a reason.
	Archive Read(const std::vector<uint8_t>& data, KeyPhase phase = KeyPhase::Auto);

	// Open an archive from a file without reading the whole thing.
	Archive Open(const std::string& path, KeyPhase phase = KeyPhase::Auto);

	// The content of one entry, decrypted when it needs to be.
	std::vector<uint8_t> Content(const Archive& archive, const Entry& entry);

	// The first `limit` bytes of one entry, decrypted - byte for byte the same
	// prefix Content() would return, without paying for the rest of the member.
	// This is what recognising a member costs: `pfstool list --types` wanted 512
	// bytes per member and used to ask Content() for them, which decrypted a whole
	// 202 MB volume (245 ms, 225 MiB of peak RSS) to look at 1682 signatures.
	// With a bounded sample the same answer costs 2.2 ms and 24 MiB.
	std::vector<uint8_t> Sample(const Archive& archive, const Entry& entry, size_t limit);

	// The content of one entry straight into a stream, a chunk at a time: this
	// is what extracting a large member from a large archive uses.
	bool ExtractTo(const Archive& archive, const Entry& entry, FILE* out);

	// Build an archive from name/content pairs.  `magic` selects the generation,
	// `encrypt` the XOR layer, and `phase` where the key starts.
	std::vector<uint8_t> Build(
		const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files,
		const std::string& magic = "pf8", bool encrypt = true,
		KeyPhase phase = KeyPhase::EntryRelative);

	// Hand-written pf2 archive, the layout only the reference describes: the self
	// test holds the v2 reader to this fixture instead of to our own writer.
	std::vector<uint8_t> BuildVersion2(
		const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files);

	// SHA1, in the one place it is used (the content key).
	void Sha1(const uint8_t* data, size_t length, uint8_t out[20]);

	// "root\image\a.png" -> "root/image/a.png", for comparing game paths
	std::string NormalizeName(const std::string& name);

	// The entry whose name matches, ignoring case and slash direction.
	const Entry* Find(const Archive& archive, const std::string& name);

	// The entries whose name ends with `suffix` (also matched case-insensitively).
	std::vector<const Entry*> FindSuffix(const Archive& archive, const std::string& suffix);
}

// ---------------------------------------------------------------------------
// the split spelling
// ---------------------------------------------------------------------------
//
// A shipped title keeps its data in several archives named "<game>.pfs.<NNN>",
// each an archive of its own (see docs/GAME_LAYOUT.md).  A web release is often
// a bare "root.pfs".  These list what a game has and read any one of them, so a
// caller can work from the game folder rather than from a file name it has to
// know in advance.

namespace artemis
{
	// the archive files in `directory` whose name starts with `stem` (for example
	// "Amakano3"), in suffix order; empty when there are none.  "<stem>.pfs",
	// "<stem>.pfs.001" and "<stem>.001" are recognised, as is a bare "root.pfs"
	// when `stem` is empty.
	std::vector<std::string> FindArchives(const std::string& directory, const std::string& stem);

	// the concatenated entries of every archive named, each listed on its own
	// (nothing is read beyond its index); names are prefixed with the archive
	// basename so a caller can tell them apart
	std::vector<Entry> ListAll(const std::vector<std::string>& paths);
}

#endif // ARTEMIS_PFS_H