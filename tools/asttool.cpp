// asttool - read an Artemis .ast script, and write it back
//
//   asttool dump     <script.ast>      parse it and print it back as a tree
//   asttool roundtrip <script.ast>     parse, dump, parse again, compare
//   asttool selftest                   the same, on a script this tool writes
//
// The property that matters is that a parse and a dump lose nothing: a script
// read into the tree and written out again must parse to the same tree.  That is
// what the self test holds the reader to, and it needs no game data.

#include "../src/ast.h"
#include "../src/luaast.h"
#include "../src/sniff.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <iconv.h>

// Artemis scripts are Shift-JIS in the older titles and UTF-8 in the newer ones;
// a line read as bytes has to be converted before it can be printed
static std::string ToUtf8(const std::string& in, bool fromSjis)
{
	if(!fromSjis)
		return in;
	iconv_t cd = iconv_open("UTF-8", "SHIFT_JIS");
	if(cd == (iconv_t)-1)
		return in;
	std::string out(in.size() * 3 + 4, '\0');
	char* src = const_cast<char*>(in.data());
	size_t srcLen = in.size();
	char* dst = &out[0];
	size_t dstLen = out.size();
	const size_t r = iconv(cd, &src, &srcLen, &dst, &dstLen);
	iconv_close(cd);
	if(r == (size_t)-1)
		return in;
	out.resize(out.size() - dstLen);
	return out;
}

static std::string ReadFile(const char* path)
{
	FILE* f = fopen(path, "rb");
	if(f == nullptr)
		return std::string();
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::string out((size_t)(size > 0 ? size : 0), '\0');
	if(size > 0)
		out.resize(fread(&out[0], 1, out.size(), f));
	fclose(f);
	return out;
}

// a canonical form of the tree, so two parses can be compared without relying on
// the exact whitespace a dump happens to use
static void Canon(const artemis::Node& node, std::string& out)
{
	switch(node.type)
	{
	case artemis::NodeType::String: out += "s(" + node.text + ")"; break;
	case artemis::NodeType::Integer: out += "i(" + node.text + ")"; break;
	case artemis::NodeType::Float: out += "f(" + node.text + ")"; break;
	case artemis::NodeType::Identifier: out += "n(" + node.text + ")"; break;
	case artemis::NodeType::Tag: out += "t(" + node.text + ")"; break;
	case artemis::NodeType::Dictionary:
		out += "d(";
		for(auto& m : node.members)
		{
			out += m.first + ":";
			Canon(m.second, out);
			out += ";";
		}
		out += ")";
		break;
	case artemis::NodeType::Array:
		out += "a(";
		for(auto& item : node.items)
		{
			Canon(item, out);
			out += ";";
		}
		out += ")";
		break;
	}
}

static std::string Canon(const artemis::Script& script)
{
	std::string out;
	for(auto& m : script.top)
	{
		out += m.first + ":";
		Canon(m.second, out);
		out += "\n";
	}
	return out;
}

static int RoundTrip(const std::string& text, bool verbose)
{
	try
	{
		artemis::Script first = artemis::Parse(text);
		std::string dumped = artemis::Dump(first);
		artemis::Script second = artemis::Parse(dumped);
		const std::string a = Canon(first), b = Canon(second);
		if(verbose)
		{
			printf("--- parsed %d top-level name(s) ---\n", (int)first.top.size());
			printf("%s", dumped.c_str());
		}
		if(a != b)
		{
			printf("FAIL: the tree changed across a dump and a parse\n");
			return 1;
		}
		return 0;
	}
	catch(const std::exception& e)
	{
		printf("FAIL: %s\n", e.what());
		return 1;
	}
}


// the game's lines, in the order a scenario would show them: every string that is
// not a key, plus the tags that mark pauses in them
static void CollectText(const artemis::Node& node, std::vector<std::string>& out)
{
	using artemis::NodeType;
	switch(node.type)
	{
	case NodeType::String:
		out.push_back(node.text);
		break;
	case NodeType::Tag:
		out.push_back(node.hasTagNumber ? ("[" + node.text + "]") : "[]");
		break;
	case NodeType::Array:
		for(auto& item : node.items)
			CollectText(item, out);
		break;
	case NodeType::Dictionary:
		// a member's key is not text; its value is - but `name` and the version
		// fields are bookkeeping, not something a player reads
		for(auto& m : node.members)
			if(m.first != "name" && m.first != "astver" && m.first != "version")
				CollectText(m.second, out);
		break;
	default:
		break;
	}
}

static int Text(const std::string& path, bool fromSjis)
{
	try
	{
		artemis::Script script = artemis::Parse(ReadFile(path.c_str()));
		std::vector<std::string> lines;
		// the lines live under `text`; a script without one (a system file) has
		// nothing to show
		for(auto& m : script.top)
			if(m.first == "text")
				CollectText(m.second, lines);
		printf("%s: %d line(s)\n", path.c_str(), (int)lines.size());
		for(auto& l : lines)
			printf("  | %s\n", ToUtf8(l, fromSjis).c_str());
		return 0;
	}
	catch(const std::exception& e)
	{
		fprintf(stderr, "asttool: %s\n", e.what());
		return 1;
	}
}

// ---------------------------------------------------------------- real scripts
//
// The real .ast is a Lua table (src/luaast.h), not the invented text format
// above.  `real` reads it: the two top-level layouts, the commands, the dialogue
// under `text.ja`, the voice references, the linknext chain.

// the player-visible lines of a real script, printed the way `text` prints them
static int TextReal(const std::string& path, bool fromSjis)
{
	const std::string text = ReadFile(path.c_str());
	artemis::LuaAst ast;
	std::string error;
	if(!artemis::ParseLuaAst(text, ast, error))
	{
		fprintf(stderr, "asttool: %s: %s\n", path.c_str(), error.c_str());
		return 1;
	}
	const std::vector<artemis::SpokenLine> spoken = artemis::SpokenLines(ast);
	printf("%s: %d line(s)\n", path.c_str(), (int)spoken.size());
	for(const artemis::SpokenLine& line : spoken)
		printf("  | %s%s%s\n", line.speaker.empty() ? "" : ("[" + ToUtf8(line.speaker, fromSjis) + "] ").c_str(),
			ToUtf8(line.text, fromSjis).c_str(),
			line.block.empty() ? "" : ("   (" + line.block + ")").c_str());
	return 0;
}

static int Real(const std::string& path, bool fromSjis, bool listCommands, int limit)
{
	const std::string text = ReadFile(path.c_str());
	if(text.empty())
	{
		fprintf(stderr, "asttool: cannot read %s\n", path.c_str());
		return 1;
	}
	artemis::LuaAst ast;
	std::string error;
	if(!artemis::ParseLuaAst(text, ast, error))
	{
		fprintf(stderr, "asttool: %s: %s\n", path.c_str(), error.c_str());
		return 1;
	}
	size_t commands = 0, lines = 0, voices = 0;
	int linked = 0;
	for(const artemis::AstBlock& b : ast.blocks)
	{
		commands += b.commands.size();
		lines += b.lines.size();
		voices += b.voices.size();
		if(!b.next.empty())
			linked++;
	}
	printf("%s: %s layout%s, %d block(s), %d command(s), %d line(s), %d voice ref(s)\n",
		path.c_str(), ast.arrayed ? "arrayed (dialogue external)" : "chained",
		ast.version.empty() ? "" : (" astver " + ast.version).c_str(),
		(int)ast.blocks.size(), (int)commands, (int)lines, (int)voices);
	if(linked > 0)
		printf("  %d block(s) carry a linknext\n", linked);
	for(const std::string& note : ast.notes)
		printf("  note: %s\n", note.c_str());

	if(listCommands)
	{
		std::map<std::string, int> counts;
		for(const artemis::AstBlock& b : ast.blocks)
			for(const artemis::AstCommand& c : b.commands)
				counts[c.name]++;
		printf("  %d distinct command(s):\n", (int)counts.size());
		for(const auto& c : counts)
			printf("    %-16s %d\n", c.first.c_str(), c.second);
	}

	// block summary, then the lines themselves
	for(size_t i = 0; i < ast.blocks.size() && i < 6; i++)
	{
		const artemis::AstBlock& b = ast.blocks[i];
		printf("  block %-14s commands %-4d lines %-4d%s\n", b.name.c_str(),
			(int)b.commands.size(), (int)b.lines.size(),
			b.next.empty() ? "" : (" -> " + b.next).c_str());
	}
	if(ast.blocks.size() > 6)
		printf("  ... %d more block(s)\n", (int)(ast.blocks.size() - 6));

	const std::vector<artemis::SpokenLine> spoken = artemis::SpokenLines(ast);
	printf("  first %d of %d spoken line(s):\n", (int)std::min((size_t)limit, spoken.size()),
		(int)spoken.size());
	for(size_t i = 0; i < spoken.size() && i < (size_t)limit; i++)
		printf("    %s\n      %s%s\n", spoken[i].block.c_str(),
			spoken[i].speaker.empty() ? "" : ("[" + ToUtf8(spoken[i].speaker, fromSjis) + "] ").c_str(),
			ToUtf8(spoken[i].text, fromSjis).c_str());
	return 0;
}

// the real samples this repository ships; a self test uses them when they are
// there, so "green" means green on real data too, not only on a fixture
#ifdef ARTEMIS_TESTDATA
static const char* kTestData = ARTEMIS_TESTDATA;
#else
static const char* kTestData = "../testdata";
#endif

static std::string SamplePath(const char* relative)
{
	return std::string(kTestData) + "/" + relative;
}

// a canonical form of what the real parser produced, so "roundtrip" on a real
// script means something even though this tree has no writer for that layout
static std::string CanonReal(const artemis::LuaAst& ast)
{
	std::string out = ast.arrayed ? "arrayed;" : "chained;";
	out += "astver=" + ast.version + ";";
	for(const artemis::AstBlock& b : ast.blocks)
	{
		out += b.name + ":" + b.next + ":" + b.lang + "{";
		for(const artemis::AstCommand& c : b.commands)
		{
			out += c.name;
			for(const artemis::AstAttribute& a : c.attributes)
				out += "|" + a.name + "=" + a.value;
			out += ";";
		}
		for(const artemis::AstLine& l : b.lines)
			out += ">" + l.speaker + ":" + l.text + ";";
		out += "}";
	}
	return out;
}

// the CLI itself, run as a child process: a self test that calls the parser
// directly cannot catch a subcommand still wired to the old reader, which is
// exactly the bug this replaced
static bool RunChild(const std::string& command, std::string& output, int& status)
{
	FILE* pipe = popen((command + " 2>&1").c_str(), "r");
	if(pipe == nullptr)
		return false;
	char buffer[4096];
	size_t got = 0;
	while((got = fread(buffer, 1, sizeof(buffer), pipe)) > 0)
		output.append(buffer, got);
	const int rc = pclose(pipe);
	status = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
	return true;
}

static const char* RealSamples[] = {
	"artemis-henpri-trial/extracted/script/d01_c01_2.ast",
	"artemis-henpri-trial/extracted/script/d05_g02_3_5.ast",
	"artemis-raspberry-web/samples/script_オープニング続き.ast",
};

static int SelfTest(const char* argv0)
{
	// what a scenario looks like: a version, a nested text block with a label, a
	// quoted line, an integer and one of the position markers
	const std::string script =
		"astver = \"2.0\"\n"
		"text = {\n"
		"  \"start\" = {\n"
		"    name = \"start\"\n"
		"    lines = {\n"
		"      \"俺たちの新しい日常が、ここから始まる。\"\n"
		"      [12]\n"
		"      \"二行目\"\n"
		"    }\n"
		"    volume = 128\n"
		"    speed = 1.5\n"
		"  }\n"
		"}\n";
	printf("script: %d byte(s)\n", (int)script.size());
	int failures = RoundTrip(script, false);
	// and the file a game ships may have no trailing newline and CRLF endings
	std::string crlf = script;
	for(size_t i = 0; i < crlf.size(); i++)
		if(crlf[i] == '\n')
			crlf.insert(i, 1, '\r'), i++;
	failures += RoundTrip(crlf, false);
	failures += RoundTrip("astver = \"1.0\"\n", false);

	// the real syntax, as a fixture: floats, negative numbers, a bracketed numeric
	// key, a long string [[...]], true, and the two-level text table
	printf("real Lua-table scripts\n");
	{
		const std::string chained =
			"astver = 2.0\n"
			"ast = {\n"
			"\tblock_00000 = {\n"
			"\t\t{\"savetitle\", text=\"fixture\"},\n"
			"\t\t{\"bg\", lv=9, id=9, file=\"cinema01\", time=0, path=\":sys/\"},\n"
			"\t\t{\"text\"},\n"
			"\t\ttext = {\n"
			"\t\t\tvo = { {\"vo\", file=\"bmbob0003\", ch=\"BmboB\"} },\n"
			"\t\t\tja = { { name = {\"モブ男B\"}, [[「お前がやったんだろう！？\"]], {\"rt2\"} } },\n"
			"\t\t},\n"
			"\t\tdelay = { [600] = { {\"bg\", file=\"piicg05_2_2l\", id=7, time=-300} } },\n"
			"\t\tlinknext = \"block_00001\",\n"
			"\t\tline = 20,\n"
			"\t},\n"
			"\tblock_00001 = { {\"text\"}, text = { ja = { { \"二行目\" } } } },\n"
			"}\n";
		artemis::LuaAst ast;
		std::string error;
		const bool ok = artemis::ParseLuaAst(chained, ast, error);
		printf("  %-58s %s\n", "chained layout parses", ok ? "ok" : "FAILED");
		if(!ok)
		{
			printf("      %s\n", error.c_str());
			failures++;
		}
		else
		{
			auto check = [&](bool good, const char* what) {
				printf("  %-58s %s\n", what, good ? "ok" : "FAILED");
				if(!good)
					failures++;
			};
			check(ast.version == "2.0", "  astver 2.0 kept as written");
			check(!ast.arrayed && ast.blocks.size() == 2, "  chained layout, two blocks");
			check(ast.blocks.size() == 2 && ast.blocks[0].commands.size() == 3,
				"  three commands in the first block");
			check(ast.blocks.size() == 2 && ast.blocks[0].next == "block_00001",
				"  linknext recorded");
			const std::vector<artemis::SpokenLine> spoken = artemis::SpokenLines(ast);
			check(spoken.size() == 2, "  two spoken lines");
			if(spoken.size() == 2)
			{
				check(spoken[0].speaker == "モブ男B", "  speaker from the name table");
				check(spoken[0].text.find("お前がやったんだろう") != std::string::npos,
					"  long-bracket dialogue with a half-width quote");
				check(ast.blocks[0].voices.size() == 1, "  voice reference kept");
			}
		}

		const std::string arrayed =
			"ast = {\n"
			"  { {\"savetitle\",text=\"忠犬大将\"}, {\"bgm\", file=\"bgm001\"}, {\"text\"}, "
			"crc=\"e2d16723\", lang=\"lang_00000\" },\n"
			"}\n";
		artemis::LuaAst external;
		const bool ok2 = artemis::ParseLuaAst(arrayed, external, error);
		auto check = [&](bool good, const char* what) {
			printf("  %-58s %s\n", what, good ? "ok" : "FAILED");
			if(!good)
				failures++;
		};
		check(ok2, "arrayed layout parses");
		if(ok2)
		{
			check(external.arrayed && external.blocks.size() == 1, "  one block, arrayed");
			check(external.blocks[0].commands.size() == 3, "  three commands (bgm, text marker)");
			check(external.blocks[0].lang == "lang_00000", "  external lang file recorded");
		}
	}

	printf("the real scripts this repository ships\n");
	{
		int seen = 0;
		for(const char* sample : RealSamples)
		{
			const std::string text = ReadFile(SamplePath(sample).c_str());
			if(text.empty())
			{
				printf("  %-58s skipped (not present)\n", sample);
				continue;
			}
			seen++;
			artemis::LuaAst ast;
			std::string error;
			const bool ok = artemis::ParseLuaAst(text, ast, error);
			size_t commands = 0, lines = 0;
			for(const artemis::AstBlock& b : ast.blocks)
			{
				commands += b.commands.size();
				lines += b.lines.size();
			}
			char label[256];
			snprintf(label, sizeof(label), "%s: %d block(s), %d command(s), %d line(s)",
				sample, (int)ast.blocks.size(), (int)commands, (int)lines);
			printf("  %-58s %s\n", label, ok && !ast.blocks.empty() ? "ok" : "FAILED");
			if(!ok || ast.blocks.empty())
			{
				printf("      %s\n", error.c_str());
				failures++;
			}
		}
		if(seen == 0)
			printf("  (no real sample beside this build: fixture coverage only)\n");
	}

	printf("the same files through the command line, not the library\n");
	{
		for(size_t i = 0; i < sizeof(RealSamples) / sizeof(RealSamples[0]); i++)
		{
			const std::string path = SamplePath(RealSamples[i]);
			if(ReadFile(path.c_str()).empty())
			{
				printf("  %-58s skipped (not present)\n", RealSamples[i]);
				continue;
			}
			// `dump` must pick the real parser from the bytes: this is the exact
			// command the independent verifier runs
			std::string output;
			int status = -1;
			const bool ran = RunChild(std::string(argv0) + " dump '" + path + "'", output, status);
			const bool ok = ran && status == 0 && output.find("block") != std::string::npos;
			char label[256];
			snprintf(label, sizeof(label), "dump %s (exit %d)", RealSamples[i], status);
			printf("  %-58s %s\n", label, ok ? "ok" : "FAILED");
			if(!ok)
			{
				printf("      %s\n", output.substr(0, 200).c_str());
				failures++;
			}
			// and `text` must show dialogue from the same file
			std::string textOut;
			int textStatus = -1;
			const bool textRan = RunChild(std::string(argv0) + " text '" + path + "'", textOut,
				textStatus);
			const bool textOk = textRan && textStatus == 0 && textOut.find("line(s)") != std::string::npos;
			snprintf(label, sizeof(label), "text %s (exit %d)", RealSamples[i], textStatus);
			printf("  %-58s %s\n", label, textOk ? "ok" : "FAILED");
			if(!textOk)
			{
				printf("      %s\n", textOut.substr(0, 200).c_str());
				failures++;
			}
		}
	}

	printf(failures == 0 ? "all passed\n" : "FAILED\n");
	return failures == 0 ? 0 : 1;
}

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		fprintf(stderr, "usage: %s dump <script.ast>\n       %s text <script.ast> [--sjis]\n"
			"       %s roundtrip <script.ast>\n       %s selftest\n",
			argv[0], argv[0], argv[0], argv[0]);
		return 2;
	}
	const std::string cmd(argv[1]);
	if(cmd == "selftest")
		return SelfTest(argv[0]);
	if(argc < 3)
	{
		fprintf(stderr, "asttool: %s needs a file\n", cmd.c_str());
		return 2;
	}
	if(cmd == "real")
	{
		bool sjis = false, commands = false;
		int limit = 12;
		for(int i = 3; i < argc; i++)
		{
			const std::string arg(argv[i]);
			if(arg == "--sjis")
				sjis = true;
			else if(arg == "--commands")
				commands = true;
			else if(arg == "--limit" && i + 1 < argc)
				limit = atoi(argv[++i]);
		}
		return Real(argv[2], sjis, commands, limit);
	}
	std::string text = ReadFile(argv[2]);
	if(text.empty())
	{
		fprintf(stderr, "asttool: cannot read %s\n", argv[2]);
		return 1;
	}
	const artemis::Sniffed sniffed = artemis::SniffBytes((const uint8_t*)text.data(),
		text.size());
	const bool real = sniffed.kind == "ast-lua";
	if(cmd == "text")
	{
		const bool sjis = argc >= 4 && std::string(argv[3]) == "--sjis";
		if(real)
			return TextReal(argv[2], sjis);
		return Text(argv[2], sjis);
	}
	if(cmd == "roundtrip" && real)
	{
		// no writer exists for the real layout, so a roundtrip here means the
		// parser is deterministic: parse twice, compare a canonical form
		artemis::LuaAst first, second;
		std::string error;
		if(!artemis::ParseLuaAst(text, first, error) || !artemis::ParseLuaAst(text, second, error))
		{
			printf("FAIL: %s\n", error.c_str());
			return 1;
		}
		if(CanonReal(first) != CanonReal(second))
		{
			printf("FAIL: two parses of the same real script disagree\n");
			return 1;
		}
		printf("%s: real Lua-table script, %d block(s), parsed twice identically\n"
			"  (this tree has a reader for this layout and no writer: see docs/REALDATA.md)\n",
			argv[2], (int)first.blocks.size());
		return 0;
	}
	if(cmd == "dump")
	{
		// the real scripts are Lua tables, this tree's first format is not: decide
		// from the bytes, the way the rest of the tree does
		const artemis::Sniffed sniffed = artemis::SniffBytes(
			(const uint8_t*)text.data(), text.size());
		if(sniffed.kind == "ast-lua")
			return Real(argv[2], argc >= 4 && std::string(argv[3]) == "--sjis", false, 20);
		return RoundTrip(text, true);
	}
	if(cmd == "roundtrip")
		return RoundTrip(text, false);
	fprintf(stderr, "asttool: unknown command %s\n", cmd.c_str());
	return 2;
}
