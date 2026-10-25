// excall.h - crossing from one .ast to another and coming back
//
// The corpus writes fifty-six calls and the parameter list is *only* a file:
//
//   {"excall", file="d01_c01_3"}
//   {"excall", file="d01_c04_1"}
//
// No `label=` appears in any of the 56.  But every one of the 59 scripts ends
// with a label table, which is where the entry comes from:
//
//   label = {
//       top = { block="block_00000", label=1 },
//       select = { block="block_00125", label=9 },
//   }
//
// So an excall enters `label="top"` by default and a `label=` attribute, when a
// writer adds one, overrides it.  The called script is a chain of blocks linked
// by `linknext`; the call returns to the caller's next command when a block has
// no `linknext`.  There is no explicit `return` command in the corpus (none of
// the 59 scripts has one), but the walker accepts one anyway.
//
// Calls nest: d01_c01_3 calls d01_c01_4, and the longest chain in the trial
// reaches depth 6.  The walker therefore reports depth, call/return counts and
// events, and guards against a project that calls itself (cycle counter), a
// linknext loop (visited-block set) and runaway length (command limit).
//
// The trial corpus is one volume: 32 of the 56 target files are not inside it
// (`d04_nk18_5`, `d02_ik07`, ...).  Those are reported as unresolved, not as a
// crash and not as a silent success; what the trial *can* prove is the reachable
// prefix of the call graph.  The complete return-to-caller semantics are pinned
// by a synthetic A -> B -> C chain in the self test.
//
// This file knows nothing about the .ast syntax: a caller supplies a
// ScriptLoader that turns a file name into ScriptProgram.  That keeps the walk
// testable without a parser, and lets fxtool feed it the real luaast parser.

#ifndef ARTEMIS_FX_EXCALL_H
#define ARTEMIS_FX_EXCALL_H

#include "tween.h" // AttrNameValue

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace artemis
{
namespace fx
{
	struct ScriptCommand
	{
		std::string name;
		std::vector<AttrNameValue> attrs;

		std::string Attr(const std::string& name,
			const std::string& fallback = std::string()) const;
	};

	struct ScriptBlock
	{
		std::string name;
		std::vector<ScriptCommand> commands;
		std::string next; // linknext, empty = end of this script
	};

	struct ScriptProgram
	{
		std::string file;
		std::vector<ScriptBlock> blocks;
		std::vector<std::pair<std::string, std::string>> labels; // label -> block

		const ScriptBlock* Block(const std::string& name) const;
		// label -> block; a missing label falls back to block_00000 (every script
		// has one), else returns empty.
		std::string ResolveLabel(const std::string& label) const;
	};

	// Reads one script.  Returns false and a reason when the file is not in the
	// namespace this loader serves (a missing trial-volume target, for example).
	using ScriptLoader = std::function<bool(const std::string& file, ScriptProgram& out,
		std::string& error)>;

	struct ExcallWalk
	{
		int calls = 0;          // every excall command reached
		int returns = 0;        // every call that gave control back to its caller
		int returnCommands = 0; // explicit {"return"} commands (none in the corpus)
		int unresolved = 0;     // target files the loader could not supply
		int missingLabels = 0;  // a label that resolved to no block
		int badCalls = 0;       // excall without a file
		int cycles = 0;         // a file that called itself, directly or not
		int blocks = 0;         // blocks entered
		int commands = 0;       // commands visited
		int maxDepth = 0;       // deepest nested call (entry = 0)
		int filesLoaded = 0;    // loader successes (cache misses)
		bool limitHit = false;  // the command limit stopped the walk
		std::vector<std::string> unresolvedFiles; // unique, in first-seen order
		std::vector<std::string> unresolvedReasons; // loader's words, per miss
		std::vector<std::string> events;          // command names + call/return
	};

	// Walk from `file`:`label`.  An empty label means "top".  `commandLimit`
	// bounds total work so a damaged script cannot hang a test.
	ExcallWalk WalkExcall(const std::string& file, const std::string& label,
		const ScriptLoader& loader, size_t commandLimit = 200000);
}
} // namespace artemis::fx

#endif // ARTEMIS_FX_EXCALL_H
