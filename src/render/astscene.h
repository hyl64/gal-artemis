// astscene.h - playing a shipped .ast: the block chain, the commands that put a
// picture on the stage, and the machine-readable statistics `--ast-info` prints
//
// `src/luaast.h` reads the structure and pulls the dialogue out.  This file does
// the two things a *player* needs on top of that, and nothing else:
//
//   1. the chain.  A chained .ast (astver 2.0) is not a list: every block ends
//      with `linknext = "block_NNNNN"`, so the order the reader sees is the
//      graph, not the file order.  Walking it is where a port can be wrong
//      without anybody noticing, so WalkAstChain() returns the walk *and* its
//      defects (a repeat, a dangling linknext, a block the walk never reached)
//      rather than quietly stopping at the first surprise.
//
//   2. the commands that change the picture.  `{"bg", file=, path=}` names an
//      image, `{"text"}` marks the block that has words, `text = { ja = {...} }`
//      holds them.  Those two map onto Stage::SetLayerImage / Stage::SetPage,
//      which is the whole of it: the compositing is `screen.h`'s job.
//
// Everything a command asks for that is *not* a pixel - a sound, a screen
// effect, a wait - is recorded as a SceneEvent and handed back to the caller.
// That division is deliberate: this file links neither the audio layer nor the
// effect layer, so `agame` is the only place that knows all three, and the two
// layer libraries can be half-built without breaking anything that includes
// this header.
//
// Which images this file will read is a *policy*, not a convention.  The corpus
// this is checked against is an adult title, and its `image/ev`, `image/cg` and
// `image/fg` pixels are off limits: AstImageAllowed() refuses those three
// prefixes outright, so no caller, argument or accident can put them on a
// canvas.  Backgrounds (`image/bg`), `image/thumb`, `system/` and the all-ages
// title's own image roots stay allowed.

#ifndef ARTEMIS_RENDER_ASTSCENE_H
#define ARTEMIS_RENDER_ASTSCENE_H

#include "luaast.h"
#include "screen.h"

#include <map>
#include <string>
#include <vector>

namespace artemis
{
	// -------------------------------------------------------------------------
	// the chain
	// -------------------------------------------------------------------------
	struct AstChainInfo
	{
		bool chained = true;  // false for the arrayed layout (no linknext at all)
		bool ok = false;      // no repeats, no dangling linknext, nothing unreached
		std::string error;    // why not, in one line, when !ok

		std::string head;                  // where the walk starts
		std::vector<std::string> heads;    // every block nothing links to
		std::vector<int> order;            // block indices in reading order
		std::vector<std::string> unreached;

		int withNext = 0;   // blocks carrying linknext
		int repeated = 0;   // the walk arrived at a block twice
		int broken = 0;     // linknext names a block the file does not define
		int unnamed = 0;    // chained blocks with no name (cannot be linked)
	};

	AstChainInfo WalkAstChain(const LuaAst& ast);

	// -------------------------------------------------------------------------
	// --ast-info
	// -------------------------------------------------------------------------
	struct AstInfoOptions
	{
		bool showText = false; // print the words themselves (all-ages data only)
		int preview = 3;       // how many commands and lines the first block shows
	};

	// One `key=value` per line, so a check script can grep it instead of parsing
	// prose.  The dialogue preview is masked by default: what this corpus says
	// stays out of logs and out of the repository, but "it parsed, here is how
	// much of it there is" is still provable.
	std::string FormatAstInfo(const LuaAst& ast, const AstChainInfo& chain,
		const AstInfoOptions& options);

	// A whole-corpus roll-up ("59 files, 13133 blocks, ..."), same key=value shape.
	struct AstCorpusStats
	{
		int files = 0;
		int failed = 0;
		long long blocks = 0;
		long long commands = 0;
		long long lines = 0;
		long long voices = 0;
		long long chained = 0;
		long long arrayed = 0;
		long long chainDefects = 0;
		std::map<std::string, long long> commandNames;
		std::vector<std::string> failures;
	};
	void AddToCorpus(AstCorpusStats& stats, const LuaAst& ast, const AstChainInfo& chain);
	std::string FormatAstCorpus(const AstCorpusStats& stats);

	// -------------------------------------------------------------------------
	// events: what a command asked for that is not a pixel
	// -------------------------------------------------------------------------
	struct SceneEvent
	{
		enum class Kind
		{
			Audio,  // bgm / se / vo / loop and stop commands
			Effect, // quake, flash, fade, anime ... the picture moves
			Wait,   // {"ex", func="wait", time=3000}
			Input,  // select, stop
			Other,  // recognised, reported, not acted on
		};

		Kind kind = Kind::Other;
		std::string command; // the command name, verbatim, for the report
		std::map<std::string, std::string> attrs;
		std::string file;  // the asset the command names, resolved to a logical path
		int block = 0;     // which block of the chain (reading order)
		int index = 0;     // which command inside that block
		int line = 0;      // the block's own `line` bookkeeping field
		int order = 0;     // global order within the run

		std::string Str(const std::string& key, const std::string& fallback = "") const;
		int Int(const std::string& key, int fallback = 0) const;
		bool Has(const std::string& key) const;
	};

	// -------------------------------------------------------------------------
	// where a block's words come from
	// -------------------------------------------------------------------------
	//
	// A chained .ast keeps its dialogue inline (`text = { ja = {...} }`).  The
	// *other* shipped layout keeps it in a separate `lang_NNNNN` member named by
	// the block's `lang` field, and `ParseLuaAst` says so in `LuaAst.notes` rather
	// than guessing.  Reading a block's lines through this interface instead of
	// straight out of `AstBlock::lines` is what keeps that second layout a matter
	// of supplying a different source, not of changing the player: an external
	// source gets the block (so it can see `block.lang`) and returns the lines.
	class AstLineSource
	{
	public:
		virtual ~AstLineSource() {}
		virtual std::vector<AstLine> LinesFor(const AstBlock& block) = 0;
	};

	// The inline `ja` group: what this tree's corpus has.
	class AstInlineLines : public AstLineSource
	{
	public:
		std::vector<AstLine> LinesFor(const AstBlock& block) override { return block.lines; }
	};

	// -------------------------------------------------------------------------
	// the run
	// -------------------------------------------------------------------------
	class AstRun
	{
	public:
		struct Config
		{
			AssetSource* assets = nullptr;
			TextureCache* cache = nullptr;
			Stage* stage = nullptr;
			// Where the words come from.  nullptr = the inline `ja` group; a title
			// whose dialogue lives in `lang_NNNNN` hands in its own source.
			AstLineSource* lines = nullptr;
			std::string startBlock; // "" = the chain head; else the block to start at
			int maxBlocks = 0;      // 0 = the whole chain
			int maxPages = 0;       // 0 = every page the script has
			bool stopAtBrokenChain = true;
		};

		struct Stats
		{
			int blocksWalked = 0;
			int pages = 0;
			long long commands = 0;
			long long stageOps = 0;     // layer writes that landed
			long long assetFailures = 0;
			long long policySkipped = 0; // character / event art, never read
			long long bookkeeping = 0;   // savetitle, cacheclear, ...: no pixels
			long long events = 0;
			std::map<std::string, long long> handled;  // command -> times it landed
			std::map<std::string, long long> unwired;  // command -> times it did not
			std::map<std::string, std::string> unwiredAt; // command -> block:line
			std::map<std::string, long long> policy;      // command -> times refused
			std::vector<std::string> assetFailuresText;
		};

		AstRun(const LuaAst& ast, const Config& config);

		const AstChainInfo& chain() const { return m_chain; }
		const Stats& stats() const { return m_stats; }
		bool ok() const { return m_error.empty(); }
		const std::string& error() const { return m_error; }
		bool AtEnd() const;

		// Apply the next block of the chain.  `hasPage` says whether that block
		// left words on the stage for the driver to composite.
		bool Step(PageText& page, bool& hasPage, std::string& error);

		// The block the last Step() applied, by name (or "#index" when unnamed),
		// so a report can say where a number came from.
		std::string currentBlockName() const;

		// The events since the last call: the driver consumes them (audio, fx) and
		// nothing is lost if it does not.
		std::vector<SceneEvent> TakeEvents();

	private:
		void ApplyCommand(const AstCommand& command, const AstBlock& block, int blockIndex,
			int index);
		bool ApplyBackground(const AstCommand& command, std::string& error);

		const LuaAst& m_ast;
		Config m_config;
		AstChainInfo m_chain;
		Stats m_stats;
		std::vector<SceneEvent> m_events;
		size_t m_cursor = 0;
		int m_eventOrder = 0;
		int m_blocksWithPage = 0;
		std::string m_error;
	};

	// A command's image, as a logical asset name ("image/bg/pink"), from its
	// `path` and `file`.  `:bg/` is `image/bg/`, `:sys/` is `system/image/`; an
	// unknown `:x/` drops the colon.
	std::string ResolveAstAsset(const std::string& path, const std::string& file);

	// false for image/ev, image/cg, image/fg: the one rule this port does not
	// bend, checked here rather than trusted to the caller.
	bool AstImageAllowed(const std::string& logical);
}

#endif // ARTEMIS_RENDER_ASTSCENE_H
