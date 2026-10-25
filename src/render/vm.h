// vm.h - the .iet interpreter
//
// The real scripts are not a flat list of commands: they are three layers that
// have to run in order, and the picture is only right when all three did.
//
//   1. the boot script (first.iet): sets the variables the rest reads, calls the
//      game's own Lua to register macro files, and defines the message layers
//      with [chgmsg]/[font] - the dialogue box's font, size, box and colours are
//      here, not in this tree;
//   2. the macro files it registered (macro.iet and system/*.iet): every `*name`
//      in them becomes a tag, so `[girl]` is `*girl` in system/name.iet and
//      `[bg_b]` is `*bg_b` in system/macro_effect.iet;
//   3. the scene script (scenario/scene.iet): labels, text and [rp] pages.
//
// The interpreter keeps the three in one script cache and one call stack, which
// is how a label in a macro file can [jump] to another label in the same file or
// [return] to the scene that called it.  Every tag it does not implement is
// counted by name and located, never swallowed: a compatibility claim without
// that list is a guess.

#ifndef ARTEMIS_RENDER_VM_H
#define ARTEMIS_RENDER_VM_H

#include "iet.h"
#include "screen.h"

#include <map>
#include <string>
#include <vector>

namespace artemis
{
	class Vm : public VarLookup
	{
	public:
		struct Config
		{
			AssetSource* assets = nullptr;
			Stage* stage = nullptr;
			TextureCache* cache = nullptr;
			FontLibrary* fonts = nullptr;
			std::string bootFile; // "first.iet"; empty skips the boot pass
			bool verbose = false;
		};

		// One option of a choice: the label a pick jumps to, and the words to put
		// on screen for it.  `text` is the first line under that label when the
		// script has one - the real title builds this text in Lua, which is not
		// modelled, and a report says so.
		struct Choice
		{
			std::string label;
			std::string text;
			int line = 0;
		};

		// A click region: the title screen's `[button]`s and the chapter screen's
		// `[point]`s.  The interpreter owns the geometry because only it sees
		// `[lyprop]`; the player hit-tests with it.
		struct ClickRegion
		{
			std::string id;      // the layer the plate lives on ("60.1"), "" for [point]
			int left = 0;
			int top = 0;
			int width = 0;
			int height = 0;
			std::string file;    // where a click goes
			std::string label;
			bool call = false;   // call= instead of jump=
			int threshold = 0;   // clickablethreshold: minimum alpha to count
			bool hasImage = false; // the plate's own pixels answer the alpha test
		};

		struct Event
		{
			enum class Kind
			{
				None,   // keep going
				Page,   // a page of text is on screen: the driver draws it
				Stop,   // the script ended, or [stop] parked it (the title screen)
				Select, // a choice is on screen: the player picks one
				Error,
			};
			Kind kind = Kind::None;
			std::string file;
			int line = 0;
			std::string text;
			std::string speaker;
			std::string error;
			std::vector<Choice> choices; // Select only
			std::string choiceFile;      // the script the options live in
		};

		struct Stats
		{
			long long commands = 0;     // commands executed
			long long bootCommands = 0; // of which during the boot pass
			long long pages = 0;
			long long macroCalls = 0;
			long long macroAdds = 0;
			long long luaTags = 0; // e:tag{...} executed out of the game's Lua
			long long unknown = 0;
			long long ignored = 0;
			long long inlineTags = 0;
			long long conditions = 0;
			long long conditionErrors = 0;
			long long assetLoads = 0;
			long long assetFailures = 0;
			long long waits = 0;
			long long waitMs = 0;
		};

		// -------------------------------------------------------------------
		// what the interpreter hands out instead of acting on
		// -------------------------------------------------------------------
		//
		// Sound and the effects that need a clock are the front end's business:
		// they need an audio device, a mixer or a frame time, and none of those
		// belongs in a script interpreter.  A front end attaches a sink, the
		// interpreter passes the tag over with every variable already expanded,
		// and the tag counts as *acted on* instead of as an ignored name.  With no
		// sink attached nothing changes: the tag is counted in ignoredTags() and
		// the interpreter stays a still-frame machine.
		struct TagEvent
		{
			std::string name;
			std::string file; // the script the tag is in
			int line = 0;
			std::map<std::string, std::string> attributes; // expanded values
		};

		class TagSink
		{
		public:
			virtual ~TagSink() {}
			virtual void OnTag(const TagEvent& event) = 0;
		};

		void SetTagSink(TagSink* sink) { m_sink = sink; }
		const std::map<std::string, long long>& frontEndTags() const
		{
			return m_frontEndTags;
		}

		explicit Vm(const Config& config);

		// Run the boot script from its first item; stops at the jump to the menu.
		bool Boot(std::string& error);

		// Begin a scenario at `label` (empty = from the top).
		bool Start(const std::string& file, const std::string& label, std::string& error);

		// Execute until a page is complete, the script ends, or something breaks.
		Event Next();

		std::string Get(const std::string& name) const override;

		// Restore a saved variable table (a loaded save).  Assignment only, so a
		// restore cannot re-run a var's system= side effect.
		void SetVariables(const std::map<std::string, std::string>& vars)
		{
			for(const auto& v : vars)
				m_vars[v.first] = v.second;
		}

		// Where the interpreter is, for a save: the script it is inside, the
		// label it started from and how many pages it has shown.  Restoring is
		// Start(file, label) then Next() that many times, which is why the count
		// is part of the record.
		const std::string& currentFile() const { return m_currentFile; }
		const std::string& currentLabel() const { return m_currentLabel; }

		const Stats& stats() const { return m_stats; }
		const std::map<std::string, long long>& unknownTags() const { return m_unknownTags; }
		const std::map<std::string, long long>& ignoredTags() const { return m_ignoredTags; }
		const std::map<std::string, long long>& macroUses() const { return m_macroUses; }
		const std::map<std::string, std::string>& unknownLocations() const
		{
			return m_unknownLocations;
		}
		const std::map<std::string, std::string>& ignoredLocations() const
		{
			return m_ignoredLocations;
		}
		const std::map<std::string, std::string>& variables() const { return m_vars; }
		const std::vector<ClickRegion>& clickRegions() const { return m_regions; }
		// Jump to the option the player picked.  False when the index is not one
		// the last Select event offered.
		bool ResolveChoice(int index, std::string& error);
		// Jump to a label, which is what a click on a [button] does.  The private
		// Jump() is the same thing but takes no Item; this is the way a front end
		// (which only has the region, not the tag) asks for it.
		bool JumpTo(const std::string& file, const std::string& label, std::string& error)
		{
			Jump(file, label, error);
			return error.empty();
		}
		// Whether a click on this region is a hit: the rect, and the plate's own
		// alpha against clickablethreshold when the script set one.
		bool RegionHit(int index, int x, int y) const;
		// The rectangle a region covers on the stage, for hit-testing and for the
		// report.  Image regions ask the stage (a [lyprop] may have moved the plate);
		// [point] regions carry their own numbers.
		bool RegionBox(int index, int& left, int& top, int& width, int& height) const;
		const std::vector<Choice>& lastChoices() const { return m_lastChoices; }
		std::vector<std::string> macroNames() const;
		size_t macroCount() const { return m_macros.size(); }
		std::vector<std::string> loadedScripts() const;
		const IetScript* script(const std::string& file) const;
		std::string assetFailuresText() const;
		const std::string& bootSummary() const { return m_bootSummary; }
		TagSink* tagSink() const { return m_sink; }

	private:
		struct Macro
		{
			std::string file;
			size_t index = 0;
			int line = 0;
		};

		struct Frame
		{
			std::string file;
			size_t index = 0;
			std::string macro; // empty for the entry script
			size_t chgDepth = 0;
			size_t ifDepth = 0;
			size_t paramDepth = 0;
		};

		const IetScript* Script(const std::string& file);
		bool LoadScript(const std::string& file, std::string& error); // parse + cache
		std::string CurrentFile() const;
		int CurrentLine() const;

		Event Execute(const Item& item, size_t nextIndex);
		Event FinishPage(const Item& item);
		void AppendText(const Item& item);
		std::string& TextBuffer();

		void Return(std::string& error);
		bool PushMacro(const std::string& name, const Item& item, std::string& error);
		bool PushCall(const std::string& file, const std::string& label, const Item& item,
			std::string& error);
		void Jump(const std::string& file, const std::string& label, std::string& error);

		size_t SkipToBranch(size_t start, bool& taken, std::string& error);
		size_t SkipToEndIf(size_t start);

		bool Condition(const Item& item, bool& ok);
		std::string Value(const Item& item, const std::string& attr,
			const std::string& fallback = std::string()) const;
		int IntValue(const Item& item, const std::string& attr, int fallback) const;

		Event DoVar(const Item& item);
		Event DoChgMsg(const Item& item);
		Event DoFont(const Item& item);
		Event DoLyC(const Item& item);
		Event DoLyProp(const Item& item);
		Event DoLyDel(const Item& item);
		Event DoLyColor(const Item& item);
		Event DoTrans(const Item& item);
		Event DoButton(const Item& item);
		Event DoPoint(const Item& item);
		Event DoSelect(const Item& item);
		Event DoMacroAdd(const Item& item);
		Event DoLua(const Item& item);

		bool RunLuaFunction(const std::string& function, std::string& error);
		bool ExtractLuaTags(const std::string& functionBody, std::vector<Item>& tags) const;

		void Note(const std::string& tag, const Item& item, bool ignored);
		void Warn(const std::string& message);

		Config m_config;
		std::map<std::string, IetScript> m_scripts;
		std::vector<std::string> m_scriptOrder;
		std::map<std::string, Macro> m_macros;
		std::map<std::string, long long> m_macroUses;
		std::map<std::string, std::string> m_vars;
		std::vector<std::map<std::string, std::string>> m_paramScopes;
		std::vector<Frame> m_frames;
		std::vector<bool> m_ifStack;
		std::vector<std::string> m_chgStack;
		std::string m_currentFile;  // the script the interpreter is inside
		std::string m_currentLabel; // the label Start() was given
		std::string m_textTarget = "adv";
		std::string m_pageText;
		std::string m_pageSpeaker;
		std::map<std::string, long long> m_unknownTags;
		std::map<std::string, std::string> m_unknownLocations;
		std::map<std::string, long long> m_ignoredTags;
		std::map<std::string, std::string> m_ignoredLocations;
		std::vector<std::string> m_assetFailures;
		Stats m_stats;
		bool m_booting = false;
		bool m_stopRequested = false;
		std::string m_lastTextFile;
		int m_lastTextLine = 0;
		std::string m_bootSummary;
		TagSink* m_sink = nullptr;
		std::vector<ClickRegion> m_regions;
		std::vector<Choice> m_lastChoices;
		std::string m_lastChoiceFile;
		bool m_lastChoiceCall = false;
		std::map<std::string, long long> m_frontEndTags;
	};
}

#endif // ARTEMIS_RENDER_VM_H
