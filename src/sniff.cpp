// sniff.cpp - the guesses described in sniff.h

#include "sniff.h"

#include <cstring>

namespace artemis
{
	static uint32_t LittleU32(const uint8_t* d)
	{
		return (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16
			| (uint32_t)d[3] << 24;
	}

	static bool Starts(const uint8_t* d, size_t n, const char* magic, size_t length)
	{
		return n >= length && memcmp(d, magic, length) == 0;
	}

	// Text means: no NUL, no control byte other than tab/CR/LF.  Shift-JIS and
	// UTF-8 high bytes are text too (old Artemis titles are SJIS, new ones are
	// UTF-8), but they are not enough on their own - binary noise that happens to
	// avoid control bytes is rare in 512-byte samples and is caught by the
	// signature checks above.
	static bool LooksLikeText(const uint8_t* d, size_t n, double* good_ratio)
	{
		size_t good = 0;
		for(size_t i = 0; i < n; i++)
		{
			const uint8_t b = d[i];
			if(b == 0)
				return false;
			if(b == '\t' || b == '\n' || b == '\r' || (b >= 0x20 && b < 0x7f) || b >= 0x80)
				good++;
			else
				return false; // a control byte that text does not use
		}
		if(good_ratio)
			*good_ratio = n ? (double)good / (double)n : 0.0;
		return true;
	}

	static bool Contains(const uint8_t* d, size_t n, const char* needle)
	{
		const size_t k = strlen(needle);
		if(n < k)
			return false;
		for(size_t i = 0; i + k <= n; i++)
			if(memcmp(d + i, needle, k) == 0)
				return true;
		return false;
	}

	// a line that starts (after nothing but spaces) with `c`
	static bool LineStartsWith(const uint8_t* d, size_t n, char c)
	{
		bool atLineStart = true;
		for(size_t i = 0; i < n; i++)
		{
			if(d[i] == '\n')
			{
				atLineStart = true;
				continue;
			}
			if(atLineStart)
			{
				if(d[i] == ' ' || d[i] == '\t' || d[i] == '\r')
					continue;
				if((char)d[i] == c)
					return true;
				atLineStart = false;
			}
		}
		return false;
	}

	// An INI opens with a `[section]` line, possibly after `;` comment lines and
	// blank lines (the real system.ini starts with a banner of them), and holds at
	// least one `key=value`.  A KAG tag line looks like `[bgm file="moon"]`, which
	// has content inside the brackets and so fails the section-name test.
	static bool LooksLikeIni(const uint8_t* d, size_t n)
	{
		size_t i = 0;
		while(i < n)
		{
			while(i < n && (d[i] == ' ' || d[i] == '\t' || d[i] == '\r' || d[i] == '\n'))
				i++;
			if(i >= n)
				return false;
			if(d[i] == ';')
			{
				while(i < n && d[i] != '\n')
					i++;
				continue;
			}
			break;
		}
		if(i >= n || d[i] != '[')
			return false;
		size_t j = i + 1;
		while(j < n && d[j] != ']' && d[j] != '\n')
		{
			const unsigned char c = d[j];
			const bool nameChar = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
				|| (c >= '0' && c <= '9') || c == '_' || c == ' ' || c == '-';
			if(!nameChar)
				return false;
			j++;
		}
		if(j >= n || d[j] != ']')
			return false;
		j++;
		while(j < n && (d[j] == ' ' || d[j] == '\t' || d[j] == '\r'))
			j++;
		if(j < n && d[j] != '\n')
			return false;
		for(size_t k = j; k < n; k++)
			if(d[k] == '=')
				return true;
		return false;
	}

	// a table file (csv/tbl) reads as text with a comma or tab in most lines
	static bool CommaHeavy(const uint8_t* d, size_t n)
	{
		size_t lines = 0, withSeparator = 0;
		bool inLine = false;
		for(size_t i = 0; i < n; i++)
		{
			if(d[i] == '\n' || d[i] == '\r')
			{
				if(inLine)
				{
					lines++;
					inLine = false;
				}
				continue;
			}
			if(!inLine)
			{
				inLine = true;
				withSeparator += (d[i] == ',' || d[i] == '\t' || d[i] == ';') ? 1 : 0;
			}
		}
		return lines >= 2 && withSeparator * 2 >= lines;
	}

	Sniffed SniffBytes(const uint8_t* d, size_t length)
	{
		Sniffed s;
		if(length == 0)
		{
			s.kind = "empty";
			return s;
		}
		const size_t n = length < 512 ? length : 512;

		// --- signatures a shipped Artemis title actually holds -----------------
		if(Starts(d, n, "\x89PNG\r\n\x1a\n", 8))
			return { "png", 1200, "PNG image" };
		if(Starts(d, n, "\xff\xd8\xff", 3))
			return { "jpeg", 1200, "JPEG image" };
		if(Starts(d, n, "OggS", 4))
			return { "ogg", 1200, "Ogg audio" };
		if(Starts(d, n, "RIFF", 4) && Starts(d + 8, n > 8 ? n - 8 : 0, "WAVE", 4))
			return { "wav", 1200, "RIFF/WAVE audio" };
		if(Starts(d, n, "PK\x03\x04", 4))
			return { "zip", 1200, "PKZIP" };
		if(Starts(d, n, "\x1f\x8b", 2))
			return { "gzip", 1200, "gzip" };
		if(Starts(d, n, "%PDF", 4))
			return { "pdf", 1200, "PDF" };
		if(Starts(d, n, "BM", 2))
			return { "bmp", 1000, "BMP image" };
		if(Starts(d, n, "ID3", 3) || Starts(d, n, "\xff\xfb", 2))
			return { "mp3", 1000, "MP3 audio" };
		if(Starts(d, n, "TLG5", 4) || Starts(d, n, "TLG6", 4) || Starts(d, n, "TLG0", 4))
			return { "tlg", 1200, "KiriKiri TLG image" };
		// fonts: a mis-decrypted XOR of a TTF would not survive this
		if(Starts(d, n, "\x00\x01\x00\x00", 4) || Starts(d, n, "true", 4))
			return { "ttf", 1200, "TrueType font" };
		if(Starts(d, n, "OTTO", 4))
			return { "otf", 1200, "OpenType/CFF font" };
		if(Starts(d, n, "ttcf", 4))
			return { "ttc", 1200, "TrueType collection" };
		if(Starts(d, n, "PSB\x00", 4))
		{
			const int version = n >= 6 ? (int)(d[4] | d[5] << 8) : 0;
			return { "psb", 1200, "Emote PSB, version " + std::to_string(version) };
		}
		if(n >= 3 && d[0] == 'p' && d[1] == 'f' && d[2] >= '0' && d[2] <= '9')
			return { "pfs", 1200, "Artemis archive, pf" + std::string(1, (char)d[2]) };
		// .asb, the binary script VNTranslationTools documents: "ASB\0\0" + item count
		if(Starts(d, n, "ASB\x00\x00", 5) && n >= 9)
		{
			const uint32_t items = LittleU32(d + 5);
			if(items < 20000000u)
				return { "asb", 1200, "ASB bytecode, " + std::to_string(items) + " items" };
		}

		// --- text -------------------------------------------------------------
		double ratio = 0.0;
		if(LooksLikeText(d, n, &ratio))
		{
			const int score = 400 + (int)(200.0 * ratio);

			// An INI starts with a bare [section] line (after comments);
			// a KAG script's tags are not sections
			if(LooksLikeIni(d, n))
				return { "ini", 700, "INI configuration" };
			if(Contains(d, n, "Sound Loop Information") || Starts(d, n, "#2.00", 5))
				return { "sli", 700, "sound loop information (text)" };
			if(LineStartsWith(d, n, '*') || Contains(d, n, "[lua]"))
				return { "kag-iet", 700, "KAG tag script (Shift-JIS or UTF-8)" };
			if(Contains(d, n, "astver") || Contains(d, n, "ast = {")
				|| Contains(d, n, "ast={"))
				return { "ast-lua", 700, "Artemis .ast script (Lua table)" };
			if(Contains(d, n, "ipt = {") || Contains(d, n, "ipt={")
				|| Contains(d, n, "ipt =\r\n{"))
			{
				// the four real dialects, told apart by what the table holds
				if(Contains(d, n, "anime"))
					return { "ipt-anime", 700, "IPT animation descriptor" };
				if(Contains(d, n, "\"cut\"") || Contains(d, n, "\"diff\""))
					return { "ipt-composite", 700, "IPT composite image descriptor" };
				if(Contains(d, n, "x=") && Contains(d, n, "y="))
					return { "ipt-data", 680, "IPT point/route table" };
				return { "ipt-atlas", 680, "IPT sprite-sheet rectangles" };
			}
			if(Contains(d, n, "function ") && Contains(d, n, "end"))
				return { "lua", score, "Lua system script" };
			if(Contains(d, n, "#2.00") || Starts(d, n, "# ", 2))
				return { "sli", 600, "hash-prefixed text" };
			if(CommaHeavy(d, n))
				return { "table-text", 500, "comma or tab separated table" };
			return { "text", score, "plain text" };
		}

		// --- binary, or noise -------------------------------------------------
		size_t zeros = 0;
		for(size_t i = 0; i < n; i++)
			if(d[i] == 0)
				zeros++;
		if(zeros == n)
			return { "zero", 0, "all zero bytes" };
		const int score = (int)(100.0 - 100.0 * (double)zeros / (double)n);
		return { "binary", score < 0 ? 0 : score, "unrecognised binary" };
	}

	int ScoreBytes(const uint8_t* data, size_t length, std::string* what)
	{
		const Sniffed s = SniffBytes(data, length);
		if(what)
			*what = s.kind;
		return s.score;
	}
}