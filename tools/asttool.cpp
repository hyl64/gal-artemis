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

#include <cstdio>
#include <cstring>
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

static int SelfTest()
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
		return SelfTest();
	if(argc < 3)
	{
		fprintf(stderr, "asttool: %s needs a file\n", cmd.c_str());
		return 2;
	}
	std::string text = ReadFile(argv[2]);
	if(text.empty())
	{
		fprintf(stderr, "asttool: cannot read %s\n", argv[2]);
		return 1;
	}
	if(cmd == "text")
	{
		const bool sjis = argc >= 4 && std::string(argv[3]) == "--sjis";
		return Text(argv[2], sjis);
	}
	if(cmd == "dump")
		return RoundTrip(text, true);
	if(cmd == "roundtrip")
		return RoundTrip(text, false);
	fprintf(stderr, "asttool: unknown command %s\n", cmd.c_str());
	return 2;
}
