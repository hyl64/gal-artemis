// arun - run a scenario and report what it would do
//
//   arun <script.ast>          the action stream, one line each
//   arun <script.ast> --count  how many lines, how long the pauses are
//
// This is the runtime's brain without a screen on it: every action a real window
// would perform, in order, so a scenario can be checked before there is anything
// to look at.

#include "../src/runtime.h"

#include <cstdio>
#include <cstring>
#include <string>

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

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		fprintf(stderr, "usage: %s <script.ast> [--count]\n", argv[0]);
		return 2;
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
