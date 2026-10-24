// runtime.h - the scenario engine
//
// A script's `text` block is a tree of named scenes; running it means walking
// that tree and turning it into the actions a screen would perform: show this
// line, wait this long, jump to that scene.  This is the part of an engine that
// has nothing to do with the platform, so it is written as the engine's own brain
// and can be tested without a window on it.
//
// The command set is what the format's markers describe:
//   a string      show it as a line and wait for the reader
//   [n]           a marker: n is a pause in milliseconds, or a scene index
//   a named block a scene that can be jumped to
//   goto = "x"    jump to the scene named x
//
// Anything not understood is reported as an unknown action rather than ignored,
// so a scenario that uses more of the format says so.

#ifndef ARTEMIS_RUNTIME_H
#define ARTEMIS_RUNTIME_H

#include "ast.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace artemis
{
	enum class ActionType
	{
		Line,    // put this text on screen and wait for the reader
		Wait,    // wait this many milliseconds, then continue
		Jump,    // go to the scene with this name
		End,     // the scenario is over
		Unknown, // something this runtime does not understand yet
	};

	struct Action
	{
		ActionType type = ActionType::End;
		std::string text;    // the line, the scene name, or what was not understood
		int64_t value = 0;   // the wait in milliseconds
	};

	class Scenario
	{
	public:
		// Load from script source; throws std::runtime_error if the text block is
		// missing or malformed.
		explicit Scenario(const std::string& source);

		// The scene names in the order the file declares them.
		std::vector<std::string> SceneNames() const;

		// The next action.  After End it keeps answering End.
		Action Next();

		// The scene being run, for the caller to show.
		const std::string& CurrentScene() const { return current; }

	private:
		void EnterScene(const std::string& name);

		std::vector<std::pair<std::string, Node>> scenes; // name -> block, owned
		std::deque<Action> pending;
		std::string current;
		bool finished = false;
	};
}

#endif // ARTEMIS_RUNTIME_H
