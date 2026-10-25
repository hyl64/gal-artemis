// lua.h - the Lua table dialect Artemis writes its data in
//
// The real files are not the shape this tree first assumed.  A shipped .ast
// script and every one of the four .ipt dialects are Lua tables:
//
//   ast = {
//       block_00000 = {
//           {"savetitle", text="LET'S GO"},
//           {"bg", lv=9, id=9, file="cinema01", time=0, path=":sys/"},
//           text = { ja = { { name = {"モブ男B"}, "「お前がやったんだろう！？」", {"rt2"} } } },
//           linknext = "block_00001",
//       },
//   }
//
//   ipt = { ["瑠莉"] = "0,1,166,93", }
//
// So one small parser serves both: anonymous arrays, named fields, `--` comments
// (the real psw_ui_ja_mw_map.ipt has them, with Japanese text after them),
// bracketed keys, floats (`astver = 2.0`), negative numbers, trailing commas and
// Shift-JIS bytes inside strings.  It is deliberately not a Lua interpreter:
// expressions, `#`, long strings and function calls are out, and a file that
// needs them says so instead of guessing.

#ifndef ARTEMIS_LUA_H
#define ARTEMIS_LUA_H

#include <string>
#include <utility>
#include <vector>

namespace artemis
{
	struct LuaValue
	{
		enum class Kind
		{
			Nil,
			Boolean,
			Integer,
			Number,
			String,
			Identifier, // a bare name used as a value, e.g. `nil` or an enum
			Object,
		};

		Kind kind = Kind::Nil;
		bool boolean = false;
		long long integer = 0;
		double number = 0.0;
		double asNumber = 0.0;    // integer or number, whichever was given
		std::string text;         // String contents, or the Identifier's name

		std::vector<LuaValue> items;                        // the array part
		std::vector<std::pair<std::string, LuaValue>> entries; // the named part

		bool isObject() const { return kind == Kind::Object; }
		const LuaValue* field(const std::string& name) const; // last write wins
		bool has(const std::string& name) const { return field(name) != nullptr; }

		// the first string in the array part: the real files put a command name
		// or a line of dialogue there
		const std::string* firstString() const;
		std::string summary() const; // "object: 3 fields, 5 items"
	};

	// Parse "name = { ... }" from `text`.  `rootName` receives the name.
	bool ParseLuaTable(const std::string& text, LuaValue& root, std::string& error,
		std::string* rootName = nullptr);

	const char* Name(LuaValue::Kind kind);
}

#endif // ARTEMIS_LUA_H