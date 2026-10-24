// pfstool - list, extract and create Artemis .pfs archives
//
//   pfstool list    <archive.pfs>
//   pfstool extract <archive.pfs> <directory>
//   pfstool create  <archive.pfs> <name=path> ...
//   pfstool selftest            build an archive, read it back, compare bytes
//
// The self test needs no game data: it writes an archive with this tool's own
// writer, reads it with the reader, and checks that every file comes back
// byte for byte - which is how the format understanding is held to account.

#include "../src/pfs.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>

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

static int List(const std::string& path)
{
	std::vector<uint8_t> data = ReadFile(path);
	if(data.empty())
	{
		fprintf(stderr, "pfstool: cannot read %s\n", path.c_str());
		return 1;
	}
	try
	{
		artemis::Archive a = artemis::Read(data);
		printf("%s: %s, %d file(s)\n", path.c_str(), a.magic.c_str(), (int)a.entries.size());
		for(auto& e : a.entries)
			printf("  %10u  %10u  %s\n", e.offset, e.size, e.name.c_str());
	}
	catch(const std::exception& e)
	{
		fprintf(stderr, "pfstool: %s\n", e.what());
		return 1;
	}
	return 0;
}

static int Extract(const std::string& path, const std::string& dir)
{
	std::vector<uint8_t> data = ReadFile(path);
	try
	{
		artemis::Archive a = artemis::Read(data);
		int written = 0;
		for(auto& e : a.entries)
		{
			std::string out = dir + "/" + e.name;
			for(size_t p = out.find('/', dir.size() + 1); p != std::string::npos;
				p = out.find('/', p + 1))
				mkdir(out.substr(0, p).c_str(), 0755);
			std::vector<uint8_t> content = artemis::Content(a, e);
			if(WriteFile(out, content))
				written++;
		}
		printf("%d file(s) written to %s\n", written, dir.c_str());
	}
	catch(const std::exception& e)
	{
		fprintf(stderr, "pfstool: %s\n", e.what());
		return 1;
	}
	return 0;
}

static int Create(int argc, char** argv)
{
	if(argc < 4)
	{
		fprintf(stderr, "usage: pfstool create <archive.pfs> <name=path> ...\n");
		return 2;
	}
	std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
	for(int i = 3; i < argc; i++)
	{
		std::string spec(argv[i]);
		size_t eq = spec.find('=');
		if(eq == std::string::npos)
		{
			fprintf(stderr, "pfstool: expected name=path, got %s\n", spec.c_str());
			return 2;
		}
		std::string name = spec.substr(0, eq);
		std::vector<uint8_t> content = ReadFile(spec.substr(eq + 1));
		if(content.empty())
		{
			fprintf(stderr, "pfstool: cannot read %s\n", spec.c_str() + eq + 1);
			return 1;
		}
		files.emplace_back(name, content);
	}
	std::vector<uint8_t> archive = artemis::Build(files, "pf8", true);
	if(!WriteFile(argv[2], archive))
	{
		fprintf(stderr, "pfstool: cannot write %s\n", argv[2]);
		return 1;
	}
	printf("wrote %s: %d file(s), %d byte(s)\n", argv[2], (int)files.size(), (int)archive.size());
	return 0;
}

static int SelfTest()
{
	// a text and a binary file, so the XOR path is exercised on both
	std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
	std::string text = "root/script/main.ast\0a line of script", bin(4096, '\0');
	std::vector<uint8_t> textBytes(text.begin(), text.end());
	std::vector<uint8_t> binBytes(4096);
	for(size_t i = 0; i < binBytes.size(); i++)
		binBytes[i] = (uint8_t)(i * 37 + 11);
	files.emplace_back("root/script/main.ast", textBytes);
	files.emplace_back("root/image/bg01.iet", binBytes);

	std::vector<uint8_t> archive = artemis::Build(files, "pf8", true);
	printf("built a %d byte archive\n", (int)archive.size());
	artemis::Archive a = artemis::Read(archive);
	printf("read back: %s, %d file(s)\n", a.magic.c_str(), (int)a.entries.size());
	if(a.entries.size() != files.size())
	{
		printf("FAIL: entry count\n");
		return 1;
	}
	int failures = 0;
	for(size_t i = 0; i < files.size(); i++)
	{
		std::vector<uint8_t> got = artemis::Content(a, a.entries[i]);
		const bool same = got == files[i].second;
		printf("  %-24s %6d byte(s)  %s\n", files[i].first.c_str(), (int)files[i].second.size(),
			same ? "identical" : "DIFFERENT");
		if(!same)
			failures++;
	}
	// the encryption must actually be on: a raw read of the content must differ
	bool encrypted = false;
	for(size_t i = 0; i < files[1].second.size() && i < 64; i++)
		if(archive[a.entries[1].offset + i] != files[1].second[i])
			encrypted = true;
	printf("  content encrypted in the archive: %s\n", encrypted ? "yes" : "no");
	printf(failures == 0 && encrypted ? "all passed\n" : "FAILED\n");
	return failures == 0 && encrypted ? 0 : 1;
}

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		fprintf(stderr,
			"usage: %s list <archive.pfs>\n"
			"       %s extract <archive.pfs> <directory>\n"
			"       %s create <archive.pfs> <name=path> ...\n"
			"       %s selftest\n", argv[0], argv[0], argv[0], argv[0]);
		return 2;
	}
	std::string cmd(argv[1]);
	if(cmd == "list" && argc >= 3)
		return List(argv[2]);
	if(cmd == "extract" && argc >= 4)
		return Extract(argv[2], argv[3]);
	if(cmd == "create" && argc >= 4)
		return Create(argc, argv);
	if(cmd == "selftest")
		return SelfTest();
	fprintf(stderr, "pfstool: bad arguments\n");
	return 2;
}
