// arun - run a scenario and report what it would do
//
//   arun <script.ast>          the action stream, one line each
//   arun <script.ast> --count  how many lines, how long the pauses are
//
//   arun <scene.iet> [--dump]  the real script language: labels, tag counts,
//                              the first items verbatim.  `agame` is the tool
//                              that executes and draws; this one only reads.
//
// This is the runtime's brain without a screen on it: every action a real window
// would perform, in order, so a scenario can be checked before there is anything
// to look at.  The .iet mode is the same idea for the real format - it runs the
// interpreter and prints the pages, but never creates a canvas or a font, which
// makes it the quickest way to tell an execution problem from a drawing problem.

#include "../src/runtime.h"

#include "../src/iet.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

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

static const char* Name(artemis::ActionType t)
{
	switch(t)
	{
	case artemis::ActionType::Line: return "line";
	case artemis::ActionType::Wait: return "wait";
	case artemis::ActionType::Jump: return "jump";
	case artemis::ActionType::End: return "end";
	case artemis::ActionType::Unknown: return "???";
	}
	return "?";
}

// a scenario with a pause and a jump between two scenes; the numbers below are
// what it must add up to, so the runtime is checked without a game or a file
static int SelfTest()
{
	const char* script =
		"astver = \"2.0\"\n"
		"text = {\n"
		"  \"start\" = {\n"
		"    \"first line\"\n"
		"    [800]\n"
		"    \"second line\"\n"
		"    goto = \"next\"\n"
		"  }\n"
		"  \"next\" = {\n"
		"    \"third line\"\n"
		"    [120]\n"
		"    \"last line\"\n"
		"  }\n"
		"}\n";
	try
	{
		artemis::Scenario scenario(script);
		int lines = 0, waits = 0, jumps = 0, unknown = 0;
		int64_t waited = 0;
		for(;;)
		{
			artemis::Action a = scenario.Next();
			if(a.type == artemis::ActionType::End)
				break;
			switch(a.type)
			{
			case artemis::ActionType::Line: lines++; break;
			case artemis::ActionType::Wait: waits++; waited += a.value; break;
			case artemis::ActionType::Jump: jumps++; break;
			case artemis::ActionType::Unknown: unknown++; break;
			default: break;
			}
		}
		const bool ok = lines == 4 && waits == 2 && waited == 920 && jumps == 1 && unknown == 0;
		printf("lines=%d pauses=%d waited=%lld jumps=%d unknown=%d -> %s\n",
			lines, waits, (long long)waited, jumps, unknown, ok ? "all passed" : "FAILED");
		return ok ? 0 : 1;
	}
	catch(const std::exception& e)
	{
		printf("FAIL: %s\n", e.what());
		return 1;
	}
}

// ---------------------------------------------------------------------------
// .iet mode: parse, count, show.  No canvas and no renderer here on purpose:
// `arun` is the cheap look at a real script (what tags does it use, in what
// numbers) and `agame` is the one that draws.  Keeping the renderer out means
// this tool needs no graphics framework at all.
// ---------------------------------------------------------------------------
static int RunIetParse(const std::string& path, bool dump)
{
	const std::string bytes = ReadFile(path.c_str());
	if(bytes.empty())
	{
		fprintf(stderr, "arun: cannot read %s\n", path.c_str());
		return 1;
	}
	const std::string text = artemis::DecodeScenarioText((const uint8_t*)bytes.data(),
		bytes.size());
	if(!artemis::LooksLikeUtf8((const uint8_t*)bytes.data(), bytes.size()))
		printf("encoding: Shift-JIS/cp932 (decoded to UTF-8)\n");
	artemis::IetScript script;
	std::string error;
	if(!artemis::IetScript::Parse(text, script, error))
	{
		fprintf(stderr, "arun: %s: %s\n", path.c_str(), error.c_str());
		return 1;
	}
	printf("%s: %d command(s), %d text line(s), %zu label(s), %d inline tag(s)\n",
		path.c_str(), script.commandCount(), script.textCount(), script.labels().size(),
		script.inlineTagCount());
	printf("labels:");
	for(const std::string& label : script.labels())
		printf(" %s", label.c_str());
	printf("\n");
	std::map<std::string, int> histogram;
	for(const artemis::Item& item : script.items())
		if(item.kind == artemis::Item::Kind::Command)
			histogram[item.text]++;
	std::vector<std::pair<std::string, int>> sorted(histogram.begin(), histogram.end());
	std::sort(sorted.begin(), sorted.end(), [](const std::pair<std::string, int>& a,
		const std::pair<std::string, int>& b) {
		if(a.second != b.second)
			return a.second > b.second;
		return a.first < b.first;
	});
	printf("tags (%zu distinct):\n", sorted.size());
	for(const std::pair<std::string, int>& entry : sorted)
		printf("    %-24s %5d\n", entry.first.c_str(), entry.second);
	if(dump)
	{
		int shown = 0;
		for(const artemis::Item& item : script.items())
		{
			if(shown++ >= 200)
			{
				printf("... (only the first 200 items)\n");
				break;
			}
			if(item.kind == artemis::Item::Kind::Label)
				printf("*%s\n", item.text.c_str());
			else
				printf("%s\n", item.Describe().c_str());
		}
	}
	return 0;
}

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		fprintf(stderr, "usage:\n  %s <script.ast> [--count]\n  %s <scene.iet> [--dump]\n",
			argv[0], argv[0]);
		return 2;
	}
	const std::string first = argv[1];
	if(first.size() > 4 && first.compare(first.size() - 4, 4, ".iet") == 0)
	{
		bool dump = false;
		for(int i = 2; i < argc; i++)
			if(strcmp(argv[i], "--dump") == 0)
				dump = true;
		return RunIetParse(first, dump);
	}
	if(argc >= 2 && strcmp(argv[1], "--selftest") == 0)
		return SelfTest();
	const bool countOnly = argc >= 3 && strcmp(argv[2], "--count") == 0;
	std::string source = ReadFile(argv[1]);
	if(source.empty())
	{
		fprintf(stderr, "arun: cannot read %s\n", argv[1]);
		return 1;
	}
	try
	{
		artemis::Scenario scenario(source);
		auto scenes = scenario.SceneNames();
		if(scenes.size() > 1)
		{
			printf("scenes:");
			for(auto& s : scenes)
				printf(" %s", s.empty() ? "(unnamed)" : s.c_str());
			printf("\n");
		}
		int lines = 0, waits = 0, jumps = 0, unknown = 0;
		int64_t waited = 0;
		for(;;)
		{
			artemis::Action a = scenario.Next();
			if(a.type == artemis::ActionType::End)
				break;
			switch(a.type)
			{
			case artemis::ActionType::Line: lines++; break;
			case artemis::ActionType::Wait: waits++; waited += a.value; break;
			case artemis::ActionType::Jump: jumps++; break;
			case artemis::ActionType::Unknown: unknown++; break;
			default: break;
			}
			if(!countOnly)
			{
				if(a.type == artemis::ActionType::Wait)
					printf("%-7s %lld ms   (scene %s)\n", Name(a.type), (long long)a.value,
						scenario.CurrentScene().c_str());
				else
					printf("%-7s %s   (scene %s)\n", Name(a.type), a.text.c_str(),
						scenario.CurrentScene().c_str());
			}
		}
		printf("%d line(s), %d pause(s) totalling %lld ms, %d jump(s), %d unknown\n",
			lines, waits, (long long)waited, jumps, unknown);
		return unknown > 0 ? 3 : 0;
	}
	catch(const std::exception& e)
	{
		fprintf(stderr, "arun: %s\n", e.what());
		return 1;
	}
}
