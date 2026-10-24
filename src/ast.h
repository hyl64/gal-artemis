// ast.h - the Artemis scenario script format
//
// An Artemis game's scenarios are text, not bytecode: `root/script/*.ast` holds
// a tree of assignments
//
//     astver = "2.0"
//     text = {
//         "label" = { ... }
//     }
//
// where a value is a string, an integer, a float, an identifier, a dictionary of
// further assignments, or a brace-delimited list.  Inside text, `[123]` marks a
// position the engine treats specially (a pause or a control marker), and it is
// kept as its own node so the runtime can act on it.
//
// This is the reader the runtime will stand on: it turns a script into a tree and
// writes a tree back out unchanged, which is what the self test checks.

#ifndef ARTEMIS_AST_H
#define ARTEMIS_AST_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace artemis
{
	enum class NodeType
	{
		String,
		Integer,
		Float,
		Identifier,
		Array,
		Dictionary,
		Tag, // the `[123]` markers in text
	};

	struct Node
	{
		NodeType type = NodeType::String;
		std::string text;     // String, Identifier, Float (as written)
		int64_t integer = 0;  // Integer, and Tag's number
		bool hasTagNumber = false;
		std::vector<Node> items;                        // Array
		std::vector<std::pair<std::string, Node>> members; // Dictionary, in order
	};

	struct Script
	{
		std::vector<std::pair<std::string, Node>> top; // the file's assignments, in order
	};

	// Parse a script; throws std::runtime_error with the position of a problem.
	Script Parse(const std::string& text);

	// Write a script back out in the format's own spelling.
	std::string Dump(const Script& script);
}

#endif // ARTEMIS_AST_H
