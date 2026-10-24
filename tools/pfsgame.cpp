// pfsgame - look at a game folder's archives the way a shipped title lays them out
//
//   pfsgame <game dir> [stem]      list every entry, saying which archive it came from
//
// A shipped Artemis title keeps several archives, "<stem>.pfs.<NNN>", each an
// archive of its own.  This walks them in suffix order, so a folder can be read
// without knowing the names in advance.

#include "../src/pfs.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		fprintf(stderr, "usage: %s <game dir> [stem]\n", argv[0]);
		return 2;
	}
	const std::string dir = argv[1];
	std::string stem;
	if(argc >= 3)
		stem = argv[2];
	else
	{
		// the stem is the archive name the folder shows; fall back to any file
		// ending in .pfs
		const std::vector<std::string> any = artemis::FindArchives(dir, "");
		if(!any.empty())
		{
			const std::string base = any.front().substr(any.front().find_last_of('/') + 1);
			const size_t dot = base.find(".pfs");
			stem = dot == std::string::npos ? base : base.substr(0, dot);
		}
	}
	if(stem.empty())
	{
		fprintf(stderr, "pfsgame: no .pfs archive found in %s\n", dir.c_str());
		return 1;
	}

	const std::vector<std::string> paths = artemis::FindArchives(dir, stem);
	if(paths.empty())
	{
		fprintf(stderr, "pfsgame: nothing named %s.pfs* in %s\n", stem.c_str(), dir.c_str());
		return 1;
	}
	printf("%s: %d archive(s)\n", stem.c_str(), (int)paths.size());
	for(const std::string& p : paths)
		printf("  %s\n", p.substr(p.find_last_of('/') + 1).c_str());

	const std::vector<artemis::Entry> entries = artemis::ListAll(paths);
	printf("%d entries\n", (int)entries.size());
	for(size_t i = 0; i < entries.size() && i < 40; i++)
		printf("  %10u  %s\n", entries[i].size, entries[i].name.c_str());
	if(entries.size() > 40)
		printf("  ... %d more\n", (int)(entries.size() - 40));
	return 0;
}
