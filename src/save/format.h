// format.h - the bytes of a save file
//
// The format is this tree's own, because no real `save*.dat` sample exists (the
// free releases here ship no saved game).  What *is* real is the naming and the
// semantics, and those come from the game: `save<N>.dat`, `save<N>.png`,
// `autosave.dat` (see state.h).  So the file is designed to be self-describing
// and hold-checkable, with three things that make a self test meaningful:
//
//   magic + version     so a reader refuses foreign bytes instead of guessing
//   section tags        so a field can be added without a flag day, and an
//                       unknown section is preserved rather than dropped
//   payload CRC32       so a truncated or edited file fails loudly at read time
//
// Layout, little-endian:
//
//   magic[8] "ARTSAVE1"
//   u32      version
//   u32      payload length
//   u32      crc32 of the payload
//   payload  sections: tag[4], u32 length, bytes
//
// Section tags and contents are in format.cpp; `DescribeSave` prints a state the
// way `savetool dump` shows it.

#ifndef ARTEMIS_SAVE_FORMAT_H
#define ARTEMIS_SAVE_FORMAT_H

#include "state.h"

#include <cstdint>
#include <string>
#include <vector>

namespace artemis
{
	namespace save
	{
		// Serialize a state.  Never fails; the caller decides where the bytes go.
		std::vector<uint8_t> SerializeState(const SaveState& state);

		// Parse.  `error` carries the reason for a bad magic, version, CRC or a
		// truncated section; the returned state is untouched then.
		bool DeserializeState(const uint8_t* data, size_t size, SaveState& out,
			std::string& error);

		// A human-readable rendering, used by `savetool dump`.
		std::string DescribeSave(const SaveState& state);

		uint32_t Crc32(const uint8_t* data, size_t size);
		// Lower-case hex SHA1 of any bytes: the thumbnail's integrity stamp.
		std::string Sha1Hex(const uint8_t* data, size_t size);
	}
}

#endif // ARTEMIS_SAVE_FORMAT_H