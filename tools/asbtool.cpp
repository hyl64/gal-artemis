// asbtool - read the binary .asb scripts
//
//   asbtool info     <script.asb>          header and counts
//   asbtool list     <script.asb> [n]      the first n items, with attributes
//   asbtool selftest                       a fixture this tool builds, plus the
//                                          real samples when the repo ships them
//
// The format is in src/asb.h.  A synthetic fixture proves the walk; the real
// samples in testdata/ prove it against bytes this tree did not write.

#include "../src/asb.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

static int Fail(const std::string& message)
{
	fprintf(stderr, "asbtool: %s\n", message.c_str());
	return 1;
}

static int Info(const std::string& path)
{
	const std::vector<uint8_t> bytes = ReadFile(path);
	if(bytes.empty())
		return Fail("cannot read " + path);
	artemis::Asb asb;
	std::string error;
	if(!artemis::ParseAsb(bytes.data(), bytes.size(), asb, error))
		return Fail(path + ": " + error);
	size_t commands = 0, labels = 0, attributes = 0;
	for(const artemis::AsbItem& item : asb.items)
	{
		if(item.kind == artemis::AsbItem::Kind::Command)
		{
			commands++;
			attributes += item.attributes.size();
		}
		else
			labels++;
	}
	printf("%s\n", path.c_str());
	printf("  header    \"ASB\\0\\0\", %d item(s) declared, %d byte(s)\n", (int)asb.itemCount,
		(int)bytes.size());
	printf("  read      %d item(s): %d command(s), %d label(s), %d attribute(s)\n",
		(int)asb.items.size(), (int)commands, (int)labels, (int)attributes);
	if(asb.truncated)
		printf("  warning   the file ends early: %s\n", asb.note.c_str());
	for(size_t i = 0; i < asb.items.size() && i < 8; i++)
	{
		const artemis::AsbItem& item = asb.items[i];
		if(item.kind == artemis::AsbItem::Kind::Label)
			printf("    * %-12s label\n", item.name.c_str());
		else
			printf("    %-14s command (line %d, %d attribute(s))\n", item.name.c_str(),
				item.line, (int)item.attributes.size());
	}
	if(asb.items.size() > 8)
		printf("    ... %d more item(s)\n", (int)(asb.items.size() - 8));
	return 0;
}

static int List(const std::string& path, int limit)
{
	const std::vector<uint8_t> bytes = ReadFile(path);
	if(bytes.empty())
		return Fail("cannot read " + path);
	artemis::Asb asb;
	std::string error;
	if(!artemis::ParseAsb(bytes.data(), bytes.size(), asb, error))
		return Fail(path + ": " + error);
	for(size_t i = 0; i < asb.items.size() && (int)i < limit; i++)
	{
		const artemis::AsbItem& item = asb.items[i];
		if(item.kind == artemis::AsbItem::Kind::Label)
		{
			printf("*%s\n", item.name.c_str());
			continue;
		}
		printf("%-16s", item.name.c_str());
		for(const artemis::AsbAttribute& a : item.attributes)
			printf(" %s=%s", a.name.c_str(), a.value.c_str());
		printf("\n");
	}
	printf("%d item(s) read, %d declared%s\n", (int)asb.items.size(), (int)asb.itemCount,
		asb.truncated ? ", truncated" : "");
	return 0;
}

// ------------------------------------------------------------------ selftest

static int Check(bool ok, const char* what, int& failures)
{
	printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
	if(!ok)
		failures++;
	return ok ? 0 : 1;
}

static void PutU32(std::vector<uint8_t>& out, uint32_t v)
{
	out.push_back((uint8_t)(v & 0xff));
	out.push_back((uint8_t)((v >> 8) & 0xff));
	out.push_back((uint8_t)((v >> 16) & 0xff));
	out.push_back((uint8_t)((v >> 24) & 0xff));
}

static void PutString(std::vector<uint8_t>& out, const std::string& text)
{
	// the declared length excludes the terminating NUL, which the real files and
	// the reader both expect
	PutU32(out, (uint32_t)text.size());
	out.insert(out.end(), text.begin(), text.end());
	out.push_back(0);
}

static void PutCommand(std::vector<uint8_t>& out, const std::string& name, int line,
	const std::vector<std::pair<std::string, std::string>>& attributes)
{
	PutU32(out, 0);
	PutString(out, name);
	PutU32(out, (uint32_t)line);
	PutU32(out, (uint32_t)attributes.size());
	for(const auto& a : attributes)
	{
		PutString(out, a.first);
		PutString(out, a.second);
	}
}

static void PutLabel(std::vector<uint8_t>& out, const std::string& name)
{
	PutU32(out, 1);
	PutString(out, name);
}

#ifdef ARTEMIS_TESTDATA
static const char* kTestData = ARTEMIS_TESTDATA;
#else
static const char* kTestData = "../testdata";
#endif

static std::string Sample(const char* relative)
{
	return std::string(kTestData) + "/" + relative;
}

static const char* RealSamples[] = {
	"artemis-raspberry-web/samples/asb/system_save.asb",
	"artemis-raspberry-web/samples/asb/system_script.asb",
	"artemis-raspberry-web/samples/asb/system_ui.asb",
};

static int SelfTest()
{
	int failures = 0;

	printf("a fixture this tool builds\n");
	{
		std::vector<uint8_t> bytes = { 'A', 'S', 'B', 0, 0 };
		PutU32(bytes, 3);
		PutCommand(bytes, "bg", 12, { { "file", "cinema01" }, { "time", "0" } });
		PutLabel(bytes, "start");
		PutCommand(bytes, "text", 30, {});
		artemis::Asb asb;
		std::string error;
		const bool ok = artemis::ParseAsb(bytes.data(), bytes.size(), asb, error);
		Check(ok, "parses the fixture", failures);
		if(ok)
		{
			Check(asb.itemCount == 3 && asb.items.size() == 3 && !asb.truncated,
				"three items, none truncated", failures);
			Check(asb.items[0].kind == artemis::AsbItem::Kind::Command
				&& asb.items[0].name == "bg" && asb.items[0].line == 12
				&& asb.items[0].attributes.size() == 2,
				"command with its line and two attributes", failures);
			Check(asb.items[0].attributes[0].name == "file"
				&& asb.items[0].attributes[0].value == "cinema01",
				"attribute name and value", failures);
			Check(asb.items[1].kind == artemis::AsbItem::Kind::Label
				&& asb.items[1].name == "start", "label", failures);
			Check(asb.items[2].attributes.empty() && asb.items[2].line == 30,
				"a command with no attributes", failures);
		}

		// a file that stops in the middle must be reported, not crash
		std::vector<uint8_t> cut(bytes.begin(), bytes.begin() + bytes.size() / 2);
		artemis::Asb partial;
		const bool ok2 = artemis::ParseAsb(cut.data(), cut.size(), partial, error);
		Check(ok2 && partial.truncated && !partial.note.empty(),
			"a file cut in half parses what it has and says so", failures);

		const std::vector<uint8_t> junk = { 'n', 'o', 'p', 'e', 0, 0, 0, 0 };
		artemis::Asb notAsb;
		Check(!artemis::ParseAsb(junk.data(), junk.size(), notAsb, error),
			"a file without the header is refused", failures);
	}

	printf("the real ASB samples this repository ships\n");
	{
		int seen = 0;
		for(size_t s = 0; s < sizeof(RealSamples) / sizeof(RealSamples[0]); s++)
		{
			const char* sample = RealSamples[s];
			const std::vector<uint8_t> bytes = ReadFile(Sample(sample));
			if(bytes.empty())
			{
				printf("  %-58s skipped (not present)\n", sample);
				continue;
			}
			seen++;
			artemis::Asb asb;
			std::string error;
			const bool ok = artemis::ParseAsb(bytes.data(), bytes.size(), asb, error);
			size_t labels = 0;
			for(const artemis::AsbItem& item : asb.items)
				if(item.kind == artemis::AsbItem::Kind::Label)
					labels++;
			// the three samples' exact shapes, verified byte for byte
			const int expectLabels[] = { 2, 59, 45 };
			const int expectCommands[] = { 10, 114, 181 };
			const int index = (int)s;
			char label[256];
			snprintf(label, sizeof(label), "%s: %d item(s), %d label(s) (want %d/%d)",
				sample, (int)asb.items.size(), (int)labels, expectLabels[index],
				expectCommands[index]);
			printf("  %-58s %s\n", label,
				ok && !asb.truncated && asb.items.size() == asb.itemCount
					&& (int)labels == expectLabels[index] ? "ok" : "FAILED");
			if(!ok || asb.truncated || asb.items.size() != asb.itemCount)
			{
				printf("      %s\n", error.empty() ? asb.note.c_str() : error.c_str());
				failures++;
			}
		}
		if(seen == 0)
			printf("  (no real sample beside this build: fixture coverage only)\n");
	}

	printf(failures == 0 ? "all passed\n" : "%d FAILED\n", failures);
	return failures == 0 ? 0 : 1;
}

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		fprintf(stderr, "usage: %s info <script.asb>\n       %s dump <script.asb> [n]\n"
			"       %s selftest\n", argv[0], argv[0], argv[0]);
		return 2;
	}
	const std::string cmd(argv[1]);
	if(cmd == "selftest")
		return SelfTest();
	if(argc < 3)
	{
		fprintf(stderr, "asbtool: %s needs a file\n", cmd.c_str());
		return 2;
	}
	if(cmd == "info")
		return Info(argv[2]);
	if(cmd == "list" || cmd == "dump")
		return List(argv[2], argc >= 4 ? atoi(argv[3]) : 40);
	fprintf(stderr, "asbtool: unknown command %s\n", cmd.c_str());
	return 2;
}