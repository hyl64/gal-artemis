// pfstool - list, extract and create Artemis .pfs archives
//
//   pfstool list    <archive.pfs> ...          index only, nothing decrypted
//   pfstool list    --types <archive.pfs>      also sniff what each member is
//   pfstool extract <archive.pfs> <dir> [member]
//   pfstool cat     <archive.pfs> <member>     one member to stdout, streamed
//   pfstool info    <archive.pfs> [--phase X]  version, key rule, its evidence
//   pfstool volumes <dir> [stem]               every archive a game folder holds
//   pfstool create  <archive.pfs> [--magic pf8] [--phase entry|archive|none] <name=path> ...
//   pfstool selftest            build/read every version, decide every key rule
//
// The self test needs no game data: it writes archives with this tool's own
// writer (all three key conventions), reads them back, compares bytes, and reads
// a hand-written v2 fixture.  The key-phase detection is the interesting part -
// it is what real shipped archives are judged by.

#include "../src/pfs.h"
#include "../src/sniff.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

static std::vector<uint8_t> ReadFile(const std::string& path)
{
	FILE* f = fopen(path.c_str(), "rb");
	if(f == nullptr)
		return {};
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> out((size_t)(size > 0 ? size : 0));
	if(size > 0)
		out.resize(fread(out.data(), 1, out.size(), f));
	fclose(f);
	return out;
}

static bool WriteFile(const std::string& path, const std::vector<uint8_t>& data)
{
	FILE* f = fopen(path.c_str(), "wb");
	if(f == nullptr)
		return false;
	if(!data.empty())
		fwrite(data.data(), 1, data.size(), f);
	fclose(f);
	return true;
}

static void MakeDirsFor(const std::string& path)
{
	for(size_t p = path.find('/', 1); p != std::string::npos; p = path.find('/', p + 1))
		mkdir(path.substr(0, p).c_str(), 0755);
}

static int Fail(const std::string& message)
{
	fprintf(stderr, "pfstool: %s\n", message.c_str());
	return 1;
}

static artemis::Archive OpenWith(const std::string& path, const std::string& phaseName)
{
	artemis::KeyPhase phase = artemis::KeyPhase::Auto;
	if(!phaseName.empty())
		phase = artemis::PhaseFromName(phaseName);
	return artemis::Open(path, phase);
}

// ----------------------------------------------------------------------- list

static int List(const std::string& path, bool types)
{
	try
	{
		artemis::Archive a = artemis::Open(path);
		printf("%s: %s, %d file(s), key phase %s%s\n", path.c_str(), a.magic.c_str(),
			(int)a.entries.size(), artemis::Name(a.phase),
			a.encrypted() ? "" : " (plaintext)");
		for(auto& e : a.entries)
		{
			if(!types)
			{
				printf("  %10llu  %10u  %s\n", (unsigned long long)e.offset, e.size, e.name.c_str());
				continue;
			}
			std::string kind = "-";
			if(e.size > 0)
			{
				// 512 bytes is all a signature needs; asking Sample() keeps the
				// rest of the member unread (and undecrypted)
				const std::vector<uint8_t> sample = artemis::Sample(a, e, 512);
				kind = artemis::SniffBytes(sample.data(), sample.size()).kind;
			}
			printf("  %10llu  %10u  %-10s %s\n", (unsigned long long)e.offset, e.size,
				kind.c_str(), e.name.c_str());
		}
	}
	catch(const std::exception& e)
	{
		return Fail(e.what());
	}
	return 0;
}

// -------------------------------------------------------------------- extract

static int Extract(const std::string& path, const std::string& dir, const std::string& filter)
{
	// a caller who swaps the directory and the member gets a clear refusal, not
	// "0 file(s) written": game members use backslashes, output directories do not
	if(dir.find('\\') != std::string::npos || dir.find(".png") != std::string::npos)
	{
		fprintf(stderr, "pfstool: '%s' looks like a member name, not a directory; "
			"usage is extract <archive> <directory> [member]\n", dir.c_str());
		return 2;
	}
	struct stat st;
	if(stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
	{
		if(mkdir(dir.c_str(), 0755) != 0)
		{
			fprintf(stderr, "pfstool: %s is not a directory; usage is "
				"extract <archive> <directory> [member]\n", dir.c_str());
			return 2;
		}
	}
	try
	{
		artemis::Archive a = artemis::Open(path);
		int written = 0;
		for(auto& e : a.entries)
		{
			if(!filter.empty() && e.name.find(filter) == std::string::npos)
				continue;
			std::string out = dir + "/" + artemis::NormalizeName(e.name);
			MakeDirsFor(out);
			FILE* f = fopen(out.c_str(), "wb");
			if(f == nullptr || !artemis::ExtractTo(a, e, f))
			{
				if(f)
					fclose(f);
				fprintf(stderr, "pfstool: cannot write %s\n", out.c_str());
				continue;
			}
			fclose(f);
			written++;
		}
		printf("%d file(s) written to %s\n", written, dir.c_str());
	}
	catch(const std::exception& e)
	{
		return Fail(e.what());
	}
	return 0;
}

static int Cat(const std::string& path, const std::string& member)
{
	try
	{
		artemis::Archive a = artemis::Open(path);
		const artemis::Entry* e = artemis::Find(a, member);
		if(e == nullptr)
			return Fail("no member " + member + " in " + path);
		if(!artemis::ExtractTo(a, *e, stdout))
			return Fail("cannot stream " + member);
		fflush(stdout);
	}
	catch(const std::exception& e)
	{
		return Fail(e.what());
	}
	return 0;
}

// ----------------------------------------------------------------------- type
//
// One loose file, typed by its bytes.  This is what a corpus without an archive
// needs: the raspberry samples are extracted members, and the extension is not
// evidence of anything.

static int Type(const std::string& path)
{
	const std::vector<uint8_t> bytes = ReadFile(path);
	if(bytes.empty())
		return Fail("cannot read " + path);
	const size_t n = bytes.size() < 512 ? bytes.size() : 512;
	const artemis::Sniffed sniffed = artemis::SniffBytes(bytes.data(), n);
	const std::string base = path.substr(path.find_last_of('/') + 1);
	printf("%-44s %-14s score %-5d %s\n", base.c_str(), sniffed.kind.c_str(),
		sniffed.score, sniffed.note.c_str());
	return 0;
}

// ----------------------------------------------------------------------- info

static int Info(const std::string& path, const std::string& phaseName)
{
	try
	{
		artemis::KeyPhase want = phaseName.empty() ? artemis::KeyPhase::Auto
			: artemis::PhaseFromName(phaseName);
		artemis::Archive a = artemis::Open(path, want);
		printf("file        %s\n", path.c_str());
		printf("magic       %s (version %d)\n", a.magic.c_str(), a.version);
		printf("index       %u byte(s), %d entr%s\n", a.indexSize, (int)a.entries.size(),
			a.entries.size() == 1 ? "y" : "ies");
		printf("key         %s\n", a.encrypted()
			? "SHA1(index), 20 bytes" : "none (content is plaintext)");
		printf("key phase   %s", artemis::Name(a.phase));
		if(a.encrypted())
			printf("  (content score %d%s%s)", a.phaseScore,
				a.phaseGuessed ? ", guessed" : "",
				a.keyGuessed ? ", key not expected from the version" : "");
		printf("\n");
		printf("backing     %s\n", a.streaming() ? "memory map (streaming)" : "in memory");
		// what a few members sniff as under the chosen phase: the evidence
		for(size_t i = 0; i < a.entries.size() && i < 6; i++)
		{
			const artemis::Entry& e = a.entries[i];
			const std::vector<uint8_t> sample = artemis::Sample(a, e, 512);
			const artemis::Sniffed s = artemis::SniffBytes(sample.data(), sample.size());
			printf("  sample  %-10s %-40s %s\n", s.kind.c_str(),
				e.name.substr(0, 40).c_str(), s.note.c_str());
		}
	}
	catch(const std::exception& e)
	{
		return Fail(e.what());
	}
	return 0;
}

// -------------------------------------------------------------------- volumes

static int Volumes(const std::string& dir, const std::string& stem)
{
	const std::vector<std::string> paths = artemis::FindArchives(dir, stem);
	if(paths.empty())
		return Fail("no archives found in " + dir);
	printf("%d archive(s)\n", (int)paths.size());
	for(const std::string& p : paths)
	{
		const std::string base = p.substr(p.find_last_of('/') + 1);
		try
		{
			artemis::Archive a = artemis::Open(p);
			printf("  %-28s %s  %5d file(s), key phase %s\n", base.c_str(), a.magic.c_str(),
				(int)a.entries.size(), artemis::Name(a.phase));
		}
		catch(const std::exception& e)
		{
			printf("  %-28s (not an archive: %s)\n", base.c_str(), e.what());
		}
	}
	return 0;
}

// -------------------------------------------------------------------- create

static int Create(int argc, char** argv)
{
	std::string magic = "pf8";
	std::string phaseName = "entry";
	bool encrypt = true;
	std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
	for(int i = 3; i < argc; i++)
	{
		const std::string arg(argv[i]);
		if(arg == "--magic" && i + 1 < argc)
			magic = argv[++i];
		else if(arg == "--phase" && i + 1 < argc)
			phaseName = argv[++i];
		else if(arg == "--plain")
			encrypt = false;
		else if(arg == "--encrypt")
			encrypt = true;
		else if(arg.find('=') != std::string::npos)
		{
			const size_t eq = arg.find('=');
			std::vector<uint8_t> content = ReadFile(arg.substr(eq + 1));
			if(content.empty())
				return Fail("cannot read " + arg.substr(eq + 1));
			files.emplace_back(arg.substr(0, eq), content);
		}
		else
			return Fail("expected name=path, got " + arg);
	}
	if(files.empty())
	{
		fprintf(stderr, "usage: pfstool create <archive.pfs> [--magic pf6|pf8] "
			"[--phase entry|archive|none] [--plain] <name=path> ...\n");
		return 2;
	}
	try
	{
		std::vector<uint8_t> archive = artemis::Build(files, magic, encrypt,
			artemis::PhaseFromName(phaseName));
		if(!WriteFile(argv[2], archive))
			return Fail(std::string("cannot write ") + argv[2]);
		printf("wrote %s: %d file(s), %d byte(s), %s\n", argv[2], (int)files.size(),
			(int)archive.size(), magic.c_str());
	}
	catch(const std::exception& e)
	{
		return Fail(e.what());
	}
	return 0;
}

// ------------------------------------------------------------------ selftest

static std::vector<uint8_t> FakeFile(size_t size, uint8_t seed)
{
	std::vector<uint8_t> out(size);
	for(size_t i = 0; i < size; i++)
		out[i] = (uint8_t)(i * 37 + seed);
	return out;
}

static int Check(bool ok, const char* what, int& failures)
{
	printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
	if(!ok)
		failures++;
	return ok ? 0 : 1;
}

struct BuiltCase
{
	std::string magic;
	bool encrypt;
	artemis::KeyPhase writerPhase;
	artemis::KeyPhase expectPhase;
};

static int RoundtripCase(const BuiltCase& c, int& failures)
{
	std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
	// a TTF header: the same signature a real archive's font gives us, which is
	// what makes the key phase detectable at all
	std::vector<uint8_t> ttf = { 0x00, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x80 };
	const std::vector<uint8_t> ttfRest = FakeFile(1000, 7);
	ttf.insert(ttf.end(), ttfRest.begin(), ttfRest.end());
	files.emplace_back("font\\GenShin-Mono-Normal.ttf", ttf);
	files.emplace_back("script\\main.ast", std::vector<uint8_t>(
		(const uint8_t*)"astver = \"2.0\"\ntext = {\n}\n",
		(const uint8_t*)"astver = \"2.0\"\ntext = {\n}\n" + 26));
	files.emplace_back("image\\bg\\ev01.png", FakeFile(4096, 11));

	std::vector<uint8_t> archive;
	try
	{
		archive = artemis::Build(files, c.magic, c.encrypt, c.writerPhase);
	}
	catch(const std::exception& e)
	{
		printf("  build %s failed: %s\n", c.magic.c_str(), e.what());
		failures++;
		return 1;
	}
	artemis::Archive a;
	try
	{
		a = artemis::Read(archive);
	}
	catch(const std::exception& e)
	{
		printf("  read %s failed: %s\n", c.magic.c_str(), e.what());
		failures++;
		return 1;
	}
	int bad = 0;
	char label[128];
	snprintf(label, sizeof(label), "%s %s: phase detected %s (want %s)", c.magic.c_str(),
		c.encrypt ? "keyed" : "plain", artemis::Name(a.phase),
		artemis::Name(c.expectPhase));
	bad += Check(a.phase == c.expectPhase, label, failures);
	bad += Check(a.entries.size() == files.size(), "  entry count", failures);
	for(size_t i = 0; i < files.size() && i < a.entries.size(); i++)
	{
		const std::vector<uint8_t> got = artemis::Content(a, a.entries[i]);
		bad += Check(got == files[i].second, "  member bytes identical", failures);
	}
	if(c.encrypt)
	{
		bool differs = false;
		for(size_t i = 0; i < 32 && i < files[0].second.size(); i++)
			if(archive[a.entries[0].offset + i] != files[0].second[i])
				differs = true;
		bad += Check(differs, "  content is actually XORed on disk", failures);
	}
	return bad;
}

static int SelfTest()
{
	int failures = 0;

	printf("sniffing signatures the key detection depends on\n");
	{
		const uint8_t ttf[] = { 0x00, 0x01, 0x00, 0x00, 0x10, 0x00 };
		const uint8_t png[] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 0, 0 };
		const uint8_t ogg[] = { 'O', 'g', 'g', 'S', 0, 2 };
		const uint8_t asb[] = { 'A', 'S', 'B', 0, 0, 3, 0, 0, 0 };
		const uint8_t text[] = { 'a', 's', 't', 'v', 'e', 'r', ' ', '=', ' ', '"' };
		const uint8_t noise[] = { 0x91, 0x00, 0xff, 0x03, 0x88, 0x00, 0x42 };
		Check(artemis::SniffBytes(ttf, sizeof(ttf)).kind == "ttf", "  TTF header", failures);
		Check(artemis::SniffBytes(png, sizeof(png)).kind == "png", "  PNG header", failures);
		Check(artemis::SniffBytes(ogg, sizeof(ogg)).kind == "ogg", "  Ogg header", failures);
		Check(artemis::SniffBytes(asb, sizeof(asb)).kind == "asb", "  ASB header", failures);
		Check(artemis::SniffBytes(text, sizeof(text)).kind == "ast-lua", "  .ast text (Lua table)", failures);
		Check(artemis::SniffBytes(noise, sizeof(noise)).kind != "text", "  noise is not text", failures);
	}

	printf("every generation, every key rule\n");
	const BuiltCase cases[] = {
		{ "pf8", true, artemis::KeyPhase::EntryRelative, artemis::KeyPhase::EntryRelative },
		{ "pf8", true, artemis::KeyPhase::ArchiveOffset, artemis::KeyPhase::ArchiveOffset },
		{ "pf6", false, artemis::KeyPhase::None, artemis::KeyPhase::None },
		// a pf6 that a build did encrypt: the version says plaintext, the bytes
		// say otherwise, and the reader is expected to notice
		{ "pf6", true, artemis::KeyPhase::EntryRelative, artemis::KeyPhase::EntryRelative },
	};
	for(const BuiltCase& c : cases)
		RoundtripCase(c, failures);

	printf("hand-written pf2 fixture (the layout only the reference describes)\n");
	{
		std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
		files.emplace_back("root/a.txt", std::vector<uint8_t>({ 'h', 'i', '\n' }));
		files.emplace_back("root/b.bin", FakeFile(300, 3));
		try
		{
			std::vector<uint8_t> archive = artemis::BuildVersion2(files);
			artemis::Archive a = artemis::Read(archive);
			Check(a.magic == "pf2" && a.version == 2, "  reads as pf2", failures);
			Check(a.phase == artemis::KeyPhase::None, "  pf2 is plaintext", failures);
			Check(a.entries.size() == files.size(), "  entry count", failures);
			for(size_t i = 0; i < files.size() && i < a.entries.size(); i++)
				Check(artemis::Content(a, a.entries[i]) == files[i].second,
					"  member bytes identical", failures);
		}
		catch(const std::exception& e)
		{
			printf("  pf2 fixture failed: %s\n", e.what());
			failures++;
		}
	}

	printf("a game folder of several archives\n");
	{
		char dir[] = "/tmp/fmt_pfs_XXXXXX";
		if(mkdtemp(dir) == nullptr)
		{
			printf("  cannot make a temp dir\n");
			failures++;
		}
		else
		{
			const std::string base(dir);
			std::vector<std::pair<std::string, std::vector<uint8_t>>> f1, f2, f3;
			f1.emplace_back("root/a.txt", std::vector<uint8_t>({ '1' }));
			f2.emplace_back("root/b.txt", std::vector<uint8_t>({ '2', '2' }));
			f3.emplace_back("root/c.txt", std::vector<uint8_t>({ '3', '3', '3' }));
			WriteFile(base + "/Demo.pfs.001", artemis::Build(f1));
			WriteFile(base + "/Demo.pfs.002", artemis::Build(f2));
			WriteFile(base + "/Demo.pfs", artemis::Build(f3));
			WriteFile(base + "/Demo.pfs.bak", std::vector<uint8_t>({ 'j', 'u', 'n', 'k' }));
			const std::vector<std::string> found = artemis::FindArchives(base, "Demo");
			Check(found.size() == 3, "  finds Demo.pfs, .001 and .002 but not .bak", failures);
			const std::vector<artemis::Entry> all = artemis::ListAll(found);
			Check(all.size() == 3, "  lists all three members", failures);
			unlink((base + "/Demo.pfs.001").c_str());
			unlink((base + "/Demo.pfs.002").c_str());
			unlink((base + "/Demo.pfs").c_str());
			unlink((base + "/Demo.pfs.bak").c_str());
			rmdir(dir);
		}
	}

	printf("streaming a member out of a mapped archive\n");
	{
		char path[] = "/tmp/fmt_pfs_stream.XXXXXX";
		const int fd = mkstemp(path);
		if(fd < 0)
		{
			printf("  cannot make a temp file\n");
			failures++;
		}
		else
		{
			close(fd);
			std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
			files.emplace_back("root/big.bin", FakeFile(1024 * 1024 + 77, 5));
			files.emplace_back("root/small.bin", FakeFile(64, 9));
			WriteFile(path, artemis::Build(files, "pf8", true, artemis::KeyPhase::ArchiveOffset));
			try
			{
				artemis::Archive a = artemis::Open(path, artemis::KeyPhase::ArchiveOffset);
				Check(a.streaming(), "  the archive is mapped, not loaded", failures);
				const artemis::Entry* e = artemis::Find(a, "root\\big.bin");
				Check(e != nullptr, "  backslash member names are found", failures);
				if(e)
				{
					char out[] = "/tmp/fmt_pfs_out.XXXXXX";
					const int ofd = mkstemp(out);
					FILE* f = fdopen(ofd, "w+b");
					const bool ok = artemis::ExtractTo(a, *e, f);
					fclose(f);
					const std::vector<uint8_t> got = ReadFile(out);
					Check(ok && got == files[0].second, "  streamed bytes identical", failures);
					unlink(out);
				}
			}
			catch(const std::exception& e)
			{
				printf("  streaming failed: %s\n", e.what());
				failures++;
			}
			unlink(path);
		}
	}

	printf("refusing what is not an archive\n");
	{
		const std::vector<uint8_t> junk = { 'n', 'o', 'p', 'e' };
		bool threw = false;
		try
		{
			artemis::Read(junk);
		}
		catch(const std::exception&)
		{
			threw = true;
		}
		Check(threw, "  four junk bytes are rejected", failures);
	}

	printf(failures == 0 ? "all passed\n" : "%d FAILED\n", failures);
	return failures == 0 ? 0 : 1;
}

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		fprintf(stderr,
			"usage: %s list [--types] <archive.pfs> ...\n"
			"       %s extract <archive.pfs> <directory> [member]\n"
			"       %s cat <archive.pfs> <member>\n"
			"       %s type <file> ...\n"
			"       %s info <archive.pfs> [--phase auto|none|entry|archive]\n"
			"       %s volumes <directory> [stem]\n"
			"       %s create <archive.pfs> [--magic pf6|pf8] [--phase ...] <name=path> ...\n"
			"       %s selftest\n", argv[0], argv[0], argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
		return 2;
	}
	std::string cmd(argv[1]);
	if(cmd == "list")
	{
		bool types = false;
		int first = 2;
		if(argc >= 3 && std::string(argv[2]) == "--types")
		{
			types = true;
			first = 3;
		}
		if(first >= argc)
			return Fail("list needs an archive");
		int rc = 0;
		for(int i = first; i < argc; i++)
			rc |= List(argv[i], types);
		return rc;
	}
	if(cmd == "extract" && argc >= 4)
		return Extract(argv[2], argv[3], argc >= 5 ? argv[4] : "");
	if(cmd == "cat" && argc >= 4)
		return Cat(argv[2], argv[3]);
	if(cmd == "type" && argc >= 3)
	{
		int rc = 0;
		for(int i = 2; i < argc; i++)
			rc |= Type(argv[i]);
		return rc;
	}
	if(cmd == "info" && argc >= 3)
		return Info(argv[2], argc >= 5 && std::string(argv[3]) == "--phase" ? argv[4] : "");
	if(cmd == "volumes" && argc >= 3)
		return Volumes(argv[2], argc >= 4 ? argv[3] : "");
	if(cmd == "create")
		return Create(argc, argv);
	if(cmd == "selftest")
		return SelfTest();
	fprintf(stderr, "pfstool: bad arguments\n");
	return 2;
}