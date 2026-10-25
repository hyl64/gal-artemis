// state.h - what a save file has to hold
//
// The shape comes from the real title, not from taste.  `system/save_normal.iet`
// (the game's own save/load screen, CP932) writes and reads
//
//   save<N>.dat        the slot's state            ($s.savepath + '/save' + i + '.dat')
//   save<N>.png        its thumbnail                ($s.savepath + '/save' + i + '.png')
//   autosave.dat       the automatic slot, whose number is `t.autosave_slot`
//
// asks the engine for existence with `system="file_exist"`, for the timestamp
// with `system="file_update_time" ... format="yy/MM/dd hh:mm"`, draws the
// thumbnail with `lyc id=... file="save<i>.png"` plus
// `lyprop xscale="$t.savethum_width * 100 / t.layer.width"` (so the game
// *stretches* the picture into its own box, it does not preserve aspect), and
// keeps the mode in `t.saving` (0 = loading, 1 = saving) and the has-data flag
// in `t.sldata`.
//
// And `system/save.asb`, the real bytecode, gives the write order:
//
//   [label save] [saving 0=on line=2] [wt] [calllua function=sv.save]
//   [se_saveok] [repeatedly] [return]
//
// i.e. raise the "saving" flag, call the game's Lua `sv.save`, play the success
// sound, and return.  This layer owns the middle of that: the state `sv.save`
// would have written.
//
// A save is a *snapshot a script can be resumed from*, so the state carries both
// halves: where the story is (script, label, how many pages were shown) and what
// the world looked like (variables, layers, BGM, read marks, backlog).

#ifndef ARTEMIS_SAVE_STATE_H
#define ARTEMIS_SAVE_STATE_H

#include "json.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace artemis
{
	namespace save
	{
		// format.h writes this magic; a reader refuses anything else
		extern const char kMagic[8];      // "ARTSAVE1"
		extern const uint32_t kVersion;   // 1

		// one entry of the backlog ring: what the reader saw, in order
		struct BacklogEntry
		{
			std::string speaker;
			std::string text;
			std::string file; // the script it came from
			int line = 0;
			int64_t atMs = 0;

			Json ToJson() const;
			static BacklogEntry FromJson(const Json& json);
		};

		// A layer as the script left it: `lyc` set the image, `lyprop` moved,
		// scaled, faded or hid it.  Ids are the scripts' ("60.1"), and `slot` is
		// the event handler's `name="slot" data="$i"` - what a save screen needs
		// to know which slot a clicked thumbnail belongs to.
		struct LayerState
		{
			std::string id;
			std::string file;   // logical image name, empty for a colour plate
			bool visible = true;
			int left = 0, top = 0;
			int width = 0, height = 0;
			int xscale = 100, yscale = 100;
			int alpha = 255;
			uint8_t color[4] = { 255, 255, 255, 255 };
			std::string eventFile, eventLabel; // lyevent, when one was set
			int slot = -1;

			Json ToJson() const;
			static LayerState FromJson(const Json& json);
		};

		struct BgmState
		{
			std::string file; // "moon", as `[bgm file="moon"]`
			std::string path; // ":bgm/"
			bool playing = false;
			int positionMs = 0;
			int loop = -1;
			double volume = 1.0;

			Json ToJson() const;
			static BgmState FromJson(const Json& json);
		};

		struct ThumbnailInfo
		{
			std::string fileName; // "save3.png", as the macro spells it
			int width = 0, height = 0;
			std::string sha1; // of the PNG bytes, so a missing/edited file is visible

			Json ToJson() const;
			static ThumbnailInfo FromJson(const Json& json);
		};

		// Everything one slot holds.
		struct SaveState
		{
			uint32_t formatVersion = kVersion;
			int slot = 0;
			bool autosave = false;
			int64_t timestampMs = 0; // wall clock at save time
			std::string displayTime; // "yy/MM/dd hh:mm", or the macro's noexist text
			std::string gameId;      // which title the save belongs to
			std::string engineVersion;

			// --- where the story is -------------------------------------------
			std::string scriptFile; // "scenario/scene.iet"
			std::string startLabel; // the label the session started at ("tanabata")
			std::string block;      // the label the interpreter is inside, when known
			int pageIndex = 0;      // how many pages this session has shown
			std::string speaker;    // the page on screen at save time
			std::string text;

			// --- what the world looks like ------------------------------------
			std::map<std::string, std::string> vars;
			std::vector<LayerState> layers;
			BgmState bgm;
			std::vector<std::string> readKeys; // already-read markers, snapshot
			std::vector<BacklogEntry> backlog; // the ring, most recent last
			ThumbnailInfo thumbnail;

			// A one-line summary for `savetool list`.
			std::string Summary() const;
			// "yy/MM/dd hh:mm" in local time, the format the real macro asks for
			static std::string FormatTime(int64_t timestampMs);
			// the macro's noexist="--/--/-- --:--"
			static const char* kNoData;

			Json ToJson() const;
			static SaveState FromJson(const Json& json);
		};

		// Which page of a session a runner just produced: what the save screen's
		// `[lyc ... file="save<i>.png"]` will show underneath the slot number.
		struct PageFrame
		{
			std::string file; // the script it came from
			std::string label; // the label it is inside
			std::string speaker;
			std::string text;
			int pageIndex = 0;

			Json ToJson() const;
			static PageFrame FromJson(const Json& json);
		};

		// What the real data says about the save layer, in one place so the tool
		// and its self test quote the same words.
		struct SaveSemantics
		{
			std::vector<std::string> slotFiles;   // "save<N>.dat", ...
			std::vector<std::string> thumbFiles;  // "save<N>.png"
			std::string autosaveFile;             // "autosave.dat"
			std::string autosaveSlotVar;          // "t.autosave_slot"
			std::vector<std::string> writeOrder;  // from system_save.asb
			std::vector<std::string> modeVars;    // "t.saving", "t.sldata"
			std::string timeFormat;               // "yy/MM/dd hh:mm"
			std::string noExistText;              // "--/--/-- --:--"
			std::string thumbSizeVars;            // "t.savethum_width/height"
		};

		SaveSemantics DefaultSemantics();
		// Read the real macro + bytecode when the corpus is present and check the
		// default above against them; `found` says whether evidence was available,
		// `problems` lists any disagreement.
		bool VerifySemanticsAgainstRealData(const std::string& testdataRoot,
			std::vector<std::string>& problems, bool& found);
	}
}

#endif // ARTEMIS_SAVE_STATE_H