// store.h - where saves live, and the picture that goes with them
//
// The real title keeps its saves beside the game: `$s.savepath + '/save' + i +
// '.dat'` and the matching `save<i>.png`, with `autosave.dat` for the automatic
// slot (`t.autosave_slot`).  A port cannot write into the game's own directory
// - the corpora under testdata/ are read-only, and a user's install should not be
// littered with our experiments - so the root is configurable:
//
//   --save-dir <dir>, or the ATRMIS_SAVE_DIR environment variable,
//   or ~/Library/Application Support/gal-engines/artemis-saves/
//
// and every file in it uses exactly the game's names, so a real install's
// save directory works as a root with no conversion.
//
// The thumbnail is the piece the load screen actually shows.  The macro asks for
// it with the game's own scaling:
//
//   [lyc    id="...0" file="save<i>.png"]
//   [lyprop id="...0" xscale="$t.savethum_width * 100 / t.layer.width"
//                     yscale="$t.savethum_height * 100 / t.layer.height"]
//
// so the picture is *stretched* into the box (no aspect preservation), which is
// what ScaleStretch does.  Downscaling is box-filtered (a game frame is bigger
// than a save thumbnail), upscaling is nearest - both deterministic, so a test
// can assert exact pixels.

#ifndef ARTEMIS_SAVE_STORE_H
#define ARTEMIS_SAVE_STORE_H

#include "backlog.h"
#include "readmarks.h"
#include "state.h"

#include <cstdint>
#include <string>
#include <vector>

namespace artemis
{
	namespace save
	{
		// A decoded picture held in this layer's own terms, so `artemis_save`
		// compiles and links without the renderer (this layer must be usable by a
		// front end that has one, and testable by a build that does not).
		struct Frame
		{
			int width = 0;
			int height = 0;
			std::vector<uint8_t> rgba; // straight alpha
			bool empty() const { return width <= 0 || height <= 0 || rgba.empty(); }
		};

		// Stretch `frame` to exactly width x height, the way the macro does.
		Frame ScaleStretch(const Frame& frame, int width, int height);

		// Write/read a PNG through pngio (libpng).  The writer also returns the
		// SHA1 of the encoded bytes, which the save records.
		bool WriteFramePng(const std::string& path, const Frame& frame, std::string& error,
			std::string* sha1 = nullptr);
		bool ReadFramePng(const std::string& path, Frame& frame, std::string& error);

		struct SlotInfo
		{
			int slot = 0;
			bool autosave = false;
			bool exists = false;
			int64_t timestampMs = 0;
			std::string displayTime;
			std::string scriptFile;
			std::string startLabel;
			int pageIndex = 0;
			std::string speaker;
			std::string text;
			size_t vars = 0;
			size_t layers = 0;
			size_t readMarks = 0;
			size_t backlog = 0;
			bool hasThumbnail = false;
			int thumbWidth = 0, thumbHeight = 0;
			std::string thumbSha1;
			std::string missingThumbnail; // set when the .dat says one exists and it does not
			uintmax_t bytes = 0;
		};

		class SaveStore
		{
		public:
			// The directory is created (with parents) on construction; `error`
			// says why when it cannot be.
			explicit SaveStore(const std::string& root);
			bool ok() const { return m_ok; }
			const std::string& root() const { return m_root; }
			const std::string& error() const { return m_error; }

			// ~/Library/Application Support/gal-engines/artemis-saves/, or the
			// environment override ATRMIS_SAVE_DIR when it is set
			static std::string DefaultRoot();

			std::string SlotFile(int slot) const;    // save<N>.dat
			std::string ThumbFile(int slot) const;   // save<N>.png
			std::string AutoFile() const;            // autosave.dat
			std::string AutoThumbFile() const;       // autosave.png
			std::string ReadMarksFile() const;       // alreadyread.dat
			std::string BacklogFile() const;         // backlog.dat

			bool HasSlot(int slot) const;
			bool HasAutosave() const;

			// Write a numbered slot: state bytes + thumbnail.  When `frame` is not
			// empty the thumbnail is scaled to `thumbWidth` x `thumbHeight` (the
			// macro's t.savethum_*), written, hashed and recorded in the state.
			bool WriteSlot(SaveState state, int slot, const Frame* frame, int thumbWidth,
				int thumbHeight, std::string& error);
			// Same for the automatic slot; `slot` in the state is kept as the game's
			// own t.autosave_slot.
			bool WriteAutosave(SaveState state, const Frame* frame, int thumbWidth,
				int thumbHeight, std::string& error);

			bool ReadSlot(int slot, SaveState& out, std::string& error) const;
			bool ReadAutosave(SaveState& out, std::string& error) const;

			// Every slot that exists, numbered slots first, then autosave; a slot
			// whose .dat mentions a thumbnail that is not on disk is reported, not
			// hidden.
			std::vector<SlotInfo> List(int maxSlot = 99) const;

			bool EraseSlot(int slot, std::string& error);
			bool EraseAutosave(std::string& error);

			// alreadyread.dat and backlog.dat persist across slots
			bool LoadReadMarks(AlreadyRead& marks, std::string& error) const;
			bool SaveReadMarks(const AlreadyRead& marks, std::string& error) const;
			bool LoadBacklog(Backlog& ring, std::string& error) const;
			bool SaveBacklog(const Backlog& ring, std::string& error) const;

			// Reserve the write: a .dat is written to `<name>.tmp` and renamed, so a
			// reader never sees half a save
			bool WriteAtomic(const std::string& path, const std::vector<uint8_t>& bytes,
				std::string& error) const;

		private:
			bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out,
				std::string& error) const;
			SlotInfo Describe(const std::string& path, int slot, bool autosave,
				std::string& error) const;

			std::string m_root;
			bool m_ok = false;
			std::string m_error;
		};
	}
}

#endif // ARTEMIS_SAVE_STORE_H