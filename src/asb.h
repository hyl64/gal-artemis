// asb.h - the .asb script bytecode
//
// Some Artemis titles ship scripts as a binary format instead of the Lua tables
// `src/luaast.h` reads, and VNTranslationTools documents it (ArtemisAsbScript.cs,
// arcusmaximus/VNTranslationTools).  A real sample confirms the header:
//
//   41 53 42 00 00 | 0c 00 00 00 | 01 00 00 00 | 04 ...
//   "ASB\0\0"        item count    first item type
//
// an item is either
//
//   type 0  a command: u32 name length, UTF-8 name, NUL, i32 line number,
//           u32 attribute count, then that many (name, value) pairs, each a
//           u32 length + UTF-8 text + NUL
//   type 1  a label: one such string
//
// The name lengths include the terminating NUL, which is the check that makes a
// wrong offset fail loudly instead of producing plausible garbage.

#ifndef ARTEMIS_ASB_H
#define ARTEMIS_ASB_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace artemis
{
	struct AsbAttribute
	{
		std::string name;
		std::string value;
	};

	struct AsbItem
	{
		enum class Kind
		{
			Command,
			Label,
		};

		Kind kind = Kind::Command;
		std::string name; // command name, or the label text
		int line = 0;     // commands only: the script line it came from
		std::vector<AsbAttribute> attributes;
	};

	struct Asb
	{
		uint32_t itemCount = 0; // as declared in the header
		std::vector<AsbItem> items; // as far as the file could be read
		bool truncated = false;     // the file ended before the count did
		std::string note;           // why, when truncated
	};

	// Parse an .asb.  `error` is set only when the header itself is wrong; a file
	// that stops early is reported through `truncated`.
	bool ParseAsb(const uint8_t* data, size_t size, Asb& out, std::string& error);

	// The header bytes a sniffer should see, "ASB\0\0".
	bool LooksLikeAsb(const uint8_t* data, size_t size);
}

#endif // ARTEMIS_ASB_H