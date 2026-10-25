// sniff.h - what is this file?  A guess from the first few hundred bytes
//
// Two callers need this and they are the same question asked twice:
//
//   * listing a real archive (`pfstool list`) wants to say what each member is
//     without extracting it;
//   * reading an encrypted archive wants to know which XOR key phase produces
//     something recognisable, because the format does not state it and the two
//     plausible rules disagree (`src/pfs.h`).
//
// The answer is a kind ("png", "asb", "ast-text", ...) and a score: signature
// hits score high, plain text scores in the middle, high-entropy noise scores
// near zero.  Nothing here trusts the extension.

#ifndef ARTEMIS_SNIFF_H
#define ARTEMIS_SNIFF_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace artemis
{
	struct Sniffed
	{
		std::string kind;  // "png", "ogg", "asb", "ast-text", "ipt-text", "unknown" ...
		int score = 0;     // 0..1200ish; >500 means a signature or clean text
		std::string note;  // a human-readable detail, e.g. "asb, 42 items"
	};

	// Look at up to the whole sample; `length` bytes must be readable.
	Sniffed SniffBytes(const uint8_t* data, size_t length);

	// The score alone, for comparing two decryptions of the same bytes.
	int ScoreBytes(const uint8_t* data, size_t length, std::string* what = nullptr);
}

#endif // ARTEMIS_SNIFF_H