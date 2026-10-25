// luaast.h - the .ast scripts a shipped title actually ships
//
// `docs/AST.md` describes a text format this tree invented from public notes.
// The real PC scripts are Lua tables, and there are two layouts in the wild:
//
//   chained   astver = 2.0
//             ast = {
//                 block_00000 = {
//                     {"savetitle", text="..."},
//                     {"bg", lv=9, id=9, file="cinema01", time=0, path=":sys/"},
//                     {"text"},
//                     text = {
//                         vo = { {"vo", file="bmbob0003", ch="BmboB"} },
//                         ja = { { name = {"モブ男B"}, "「お前がやったんだろう！？」", {"rt2"} } },
//                     },
//                     linknext = "block_00001",
//                     line = 20,
//                 },
//             }
//
//   arrayed   ast = {
//                 {
//                     {"savetitle", text="忠犬大将"},
//                     {"bgm", file="bgm001"},
//                     {"text"},
//                     crc = "e2d16723",
//                     lang = "lang_00000",
//                 },
//             }
//
// A command is an object whose first array item is a string - the command name;
// its named fields are the parameters.  Dialogue lives under `text` grouped by
// language (`ja`, `vo`), or is held in a separate `lang_NNNNN` file named by
// `lang`.  This parses the structure and pulls those two things out; evaluating
// conditions or running the commands is a different job (see docs/REALDATA.md).

#ifndef ARTEMIS_LUAAST_H
#define ARTEMIS_LUAAST_H

#include <string>
#include <utility>
#include <vector>

namespace artemis
{
	struct AstAttribute
	{
		std::string name;
		std::string value; // numbers and strings both, as text
		bool quoted = false;
	};

	struct AstCommand
	{
		std::string name;
		std::vector<AstAttribute> attributes;
	};

	struct AstLine
	{
		std::string speaker; // the name table, joined with ',', when present
		std::string text;
		std::vector<std::string> tags; // {"rt2"} style markers after the line
	};

	struct AstBlock
	{
		std::string name;
		std::vector<AstCommand> commands;
		std::vector<AstLine> lines;      // from the `ja` group
		std::vector<std::string> voices; // from the `vo` group (file names)
		std::string next;                // linknext
		std::string lang;                // external lang_NNNNN file
		int line = 0;                    // the `line` bookkeeping field
	};

	struct LuaAst
	{
		std::string version; // astver, when present
		bool arrayed = false; // true for the lang-external layout
		std::vector<AstBlock> blocks;
		std::vector<std::string> notes;
	};

	// Parse a real .ast.  `error` carries a line and column on failure.
	bool ParseLuaAst(const std::string& text, LuaAst& out, std::string& error);

	// Just the player-visible lines, in script order, with the block they are in.
	struct SpokenLine
	{
		std::string block;
		std::string speaker;
		std::string text;
	};
	std::vector<SpokenLine> SpokenLines(const LuaAst& ast);
}

#endif // ARTEMIS_LUAAST_H