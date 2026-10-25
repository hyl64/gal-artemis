// savetool - the save layer, exercised without a window
//
// The engine's save is not "write the interpreter state somewhere": the real
// macros in the corpus show a slot is a .dat plus a thumbnail PNG, autosave is a
// separate file with its own slot number, and the already-read set and the
// backlog outlive any single slot because they are written to their own files.
// This tool drives that layer directly so every one of those claims can be
// checked from a shell.
//
//   savetool save   <dir> --slot N [--frame <png>] [--thumb WxH] [--text "…"]
//                          [--speaker "…"] [--script <f>] [--label <l>]
//                          [--var k=v ...] [--read-key k ...]
//   savetool autosave <dir> [same options]
//   savetool load   <dir> --slot N | --autosave
//   savetool list   <dir>
//   savetool erase  <dir> --slot N | --autosave
//   savetool backlog <dir> [--push "speaker|text" ...] [--dump]
//   savetool marks  <dir> [--mark key ...]
//   savetool selftest
//
// Exit code 0 on success, 1 on a bad argument or a failed check.

#include "save/backlog.h"
#include "save/format.h"
#include "save/readmarks.h"
#include "save/state.h"
#include "save/store.h"

#include <chrono>

#ifdef ARTEMIS_SAVE_VM
#include "render/screen.h"
#include "render/vm.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{
	int failures = 0;

	void Check(bool ok, const char* what, const std::string& detail = "")
	{
		if(!ok)
			failures++;
		printf("  %-58s %s%s%s\n", what, ok ? "ok" : "FAILED",
			detail.empty() ? "" : "  ", detail.c_str());
	}

	int Fail(const std::string& message)
	{
		fprintf(stderr, "savetool: %s\n", message.c_str());
		return 1;
	}

	int64_t NowMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	}

	// A save that looks like a real one: the fields the macros actually read.
	artemis::save::SaveState MakeState(int slot, bool autosave, const std::string& text,
		const std::string& speaker, int pages)
	{
		artemis::save::SaveState state;
		state.slot = slot;
		state.autosave = autosave;
		state.timestampMs = NowMs();
		state.displayTime = "26/10/25 19:30";
		state.gameId = "artemis-nicolan";
		state.engineVersion = "port-1";
		state.scriptFile = "scenario/scene.iet";
		state.startLabel = "tanabata";
		state.block = "block_00012";
		state.pageIndex = pages;
		state.speaker = speaker;
		state.text = text;
		state.vars["g.deve"] = "0";
		state.vars["t.back"] = "bgimage/";
		state.vars["g.msgspeed"] = "30";
		state.bgm.file = "bgm/moon";
		state.bgm.loop = true;
		state.bgm.volume = 200;
		return state;
	}

#ifdef ARTEMIS_SAVE_VM
	// The claim the whole layer exists for: put the interpreter down mid-scene,
	// start a new one, and pick up where it left off.  The comparison is the page
	// text, because two frames can differ in how long a transition was asked for
	// while saying exactly the same thing.
	int VmEndToEnd(const std::string& dataDir, int pages)
	{
		using namespace artemis;
		AssetSource assets;
		std::string error;
		if(!assets.AddDirectory(dataDir, error))
			return Fail(error);

		auto runFrom = [&](const std::string& label, int skip, int take,
			const std::map<std::string, std::string>* vars,
			std::vector<std::string>& out) -> bool {
			Vm::Config config;
			config.assets = &assets;
			config.bootFile = "first.iet";
			Vm vm(config);
			if(!vm.Boot(error))
				return false;
			if(!vm.Start("scenario/scene.iet", label, error))
				return false;
			if(vars != nullptr)
				vm.SetVariables(*vars);
			for(int i = 0; i < skip; i++)
				if(vm.Next().kind != Vm::Event::Kind::Page)
					return false;
			for(int i = 0; i < take; i++)
			{
				const Vm::Event event = vm.Next();
				if(event.kind != Vm::Event::Kind::Page)
					break;
				out.push_back(event.speaker + "|" + event.text);
			}
			return true;
		};

		std::vector<std::string> straight;
		if(!runFrom("", 0, pages, nullptr, straight))
			return Fail("the uninterrupted run failed: " + error);

		std::vector<std::string> firstHalf;
		if(!runFrom("", 0, pages / 2, nullptr, firstHalf))
			return Fail("the first half failed: " + error);

		// The save: where the player is, and the variables as they stand.
		artemis::save::SaveState state = MakeState(7, false, "", "", pages / 2);
		state.scriptFile = "scenario/scene.iet";
		state.startLabel = "";
		{
			Vm::Config config;
			config.assets = &assets;
			config.bootFile = "first.iet";
			Vm vm(config);
			if(!vm.Boot(error) || !vm.Start("scenario/scene.iet", "", error))
				return Fail("preparing the save failed: " + error);
			for(int i = 0; i < pages / 2; i++)
				vm.Next();
			state.vars = vm.variables();
		}

		// A new interpreter: boot, jump to the saved label, restore the variables,
		// then skip the pages already seen.
		std::vector<std::string> secondHalf;
		if(!runFrom(state.startLabel, state.pageIndex, pages - pages / 2, &state.vars,
			secondHalf))
			return Fail("the resumed run failed: " + error);

		std::vector<std::string> joined = firstHalf;
		joined.insert(joined.end(), secondHalf.begin(), secondHalf.end());
		printf("  %-58s %s  (%zu + %zu pages vs %zu straight)\n",
			"a save resumes the same pages", joined == straight ? "ok" : "FAILED",
			firstHalf.size(), secondHalf.size(), straight.size());
		if(joined != straight)
			failures++;
		for(size_t i = 0; i < joined.size() && i < straight.size(); i++)
			if(joined[i] != straight[i])
			{
				printf("      first difference at page %zu:\n        straight: %s\n        resumed : %s\n",
					i, straight[i].substr(0, 60).c_str(), joined[i].substr(0, 60).c_str());
				break;
			}
		return 0;
	}
#endif

	int List(const std::string& dir)
	{
		artemis::save::SaveStore store(dir);
		if(!store.ok())
			return Fail(store.error());
		printf("save directory: %s\n", store.root().c_str());
		const std::vector<artemis::save::SlotInfo> slots = store.List();
		if(slots.empty())
		{
			printf("  (no slots)\n");
			return 0;
		}
		for(const artemis::save::SlotInfo& s : slots)
		{
			printf("  %-9s %-14s %5d page(s)  %-22s %s\n",
				s.autosave ? "autosave" : ("slot " + std::to_string(s.slot)).c_str(),
				s.displayTime.c_str(), s.pageIndex, s.scriptFile.c_str(),
				s.text.substr(0, 40).c_str());
			printf("            vars %zu  layers %zu  read %zu  backlog %zu  thumb %s",
				s.vars, s.layers, s.readMarks, s.backlog, s.hasThumbnail ? "yes" : "no");
			if(s.hasThumbnail)
				printf(" (%dx%d sha1 %.12s)", s.thumbWidth, s.thumbHeight,
					s.thumbSha1.c_str());
			if(!s.missingThumbnail.empty())
				printf("  MISSING %s", s.missingThumbnail.c_str());
			printf("\n");
		}
		return 0;
	}

	int Dump(const std::string& dir, int slot, bool autosave)
	{
		artemis::save::SaveStore store(dir);
		if(!store.ok())
			return Fail(store.error());
		artemis::save::SaveState state;
		std::string error;
		const bool ok = autosave ? store.ReadAutosave(state, error)
			: store.ReadSlot(slot, state, error);
		if(!ok)
			return Fail(error);
		printf("%s %d\n", autosave ? "autosave" : "slot", autosave ? state.slot : slot);
		printf("  version      %u\n", state.formatVersion);
		printf("  time         %s (%lld)\n", state.displayTime.c_str(),
			(long long)state.timestampMs);
		printf("  story        %s @ %s -> %s\n", state.scriptFile.c_str(),
			state.startLabel.c_str(), state.block.c_str());
		printf("  page         %d\n", state.pageIndex);
		printf("  speaker      %s\n", state.speaker.c_str());
		printf("  text         %s\n", state.text.c_str());
		printf("  bgm          %s loop=%d volume=%d\n", state.bgm.file.c_str(),
			(int)state.bgm.loop, state.bgm.volume);
		printf("  vars         %zu\n", state.vars.size());
		for(const auto& v : state.vars)
			printf("               %s = %s\n", v.first.c_str(), v.second.c_str());
		printf("  layers       %zu\n", state.layers.size());
		printf("  read keys    %zu\n", state.readKeys.size());
		printf("  backlog      %zu\n", state.backlog.size());
		if(!state.thumbnail.fileName.empty())
			printf("  thumbnail    %dx%d sha1 %s\n", state.thumbnail.width,
				state.thumbnail.height, state.thumbnail.sha1.c_str());
		return 0;
	}

	int Save(const std::string& dir, int slot, bool autosave, const std::string& framePath,
		int thumbW, int thumbH, const std::string& text, const std::string& speaker,
		const std::vector<std::string>& vars, const std::vector<std::string>& readKeys)
	{
		artemis::save::SaveStore store(dir);
		if(!store.ok())
			return Fail(store.error());
		artemis::save::SaveState state = MakeState(slot, autosave, text, speaker, 7);
		for(const std::string& kv : vars)
		{
			const size_t at = kv.find('=');
			if(at == std::string::npos)
				return Fail("--var wants key=value, got '" + kv + "'");
			state.vars[kv.substr(0, at)] = kv.substr(at + 1);
		}
		state.readKeys = readKeys;

		artemis::save::Frame frame;
		std::string error;
		if(!framePath.empty() && !artemis::save::ReadFramePng(framePath, frame, error))
			return Fail(error);
		const artemis::save::Frame* useFrame = frame.empty() ? nullptr : &frame;
		const bool ok = autosave
			? store.WriteAutosave(state, useFrame, thumbW, thumbH, error)
			: store.WriteSlot(state, slot, useFrame, thumbW, thumbH, error);
		if(!ok)
			return Fail(error);
		printf("wrote %s (%s)\n", autosave ? store.AutoFile().c_str()
			: store.SlotFile(slot).c_str(), store.root().c_str());
		return 0;
	}

	// ---------------------------------------------------------------- selftest

	void TestRoundTrip(const std::string& dir)
	{
		printf("a slot survives being written and read back\n");
		artemis::save::SaveStore store(dir);
		Check(store.ok(), "the store opens", store.root());

		artemis::save::SaveState state = MakeState(3, false, "「だっておかしな話じゃない？」", "少女", 12);
		state.readKeys = {"scenario/scene.iet:tanabata", "scenario/scene.iet:start"};
		std::string error;
		Check(store.WriteSlot(state, 3, nullptr, 0, 0, error), "write slot 3", error);

		artemis::save::SaveState back;
		Check(store.ReadSlot(3, back, error), "read slot 3", error);
		Check(back.slot == 3, "slot number survives");
		Check(back.scriptFile == state.scriptFile, "the script path survives", back.scriptFile);
		Check(back.startLabel == state.startLabel, "the entry label survives", back.startLabel);
		Check(back.block == state.block, "the block survives", back.block);
		Check(back.pageIndex == state.pageIndex, "the page index survives");
		Check(back.speaker == state.speaker, "the speaker survives", back.speaker);
		Check(back.text == state.text, "the line survives");
		Check(back.vars.size() == state.vars.size() && back.vars["g.msgspeed"] == "30",
			"the variable table survives", std::to_string(back.vars.size()) + " var(s)");
		Check(back.bgm.file == "bgm/moon" && back.bgm.loop && back.bgm.volume == 200,
			"the background music state survives", back.bgm.file);
		Check(back.readKeys.size() == 2, "the read keys survive");
		Check(back.formatVersion == artemis::save::kVersion, "the version survives");
		Check(back.timestampMs > 0, "a timestamp is recorded");
	}

	void TestAutosaveIsSeparate(const std::string& dir)
	{
		printf("autosave is its own file, and does not overwrite a slot\n");
		artemis::save::SaveStore store(dir);
		std::string error;
		artemis::save::SaveState autoState = MakeState(9, true, "autosaved line", "語り手", 40);
		Check(store.WriteAutosave(autoState, nullptr, 0, 0, error), "write autosave", error);

		artemis::save::SaveState slot;
		Check(store.ReadSlot(3, slot, error), "slot 3 is still there", error);
		Check(slot.text != autoState.text, "autosave did not overwrite slot 3");
		Check(store.HasAutosave(), "the store reports an autosave exists");
		Check(store.HasSlot(3), "the store reports slot 3 exists");
		artemis::save::SaveState back;
		Check(store.ReadAutosave(back, error), "read autosave", error);
		Check(back.autosave, "the autosave flag is set");
		Check(back.slot == 9, "the game's own autosave slot number is kept",
			std::to_string(back.slot));
	}

	void TestThumbnail(const std::string& dir)
	{
		printf("a frame becomes a scaled thumbnail, and the record knows its hash\n");
		artemis::save::Frame frame;
		frame.width = 1024;
		frame.height = 698;
		frame.rgba.resize((size_t)frame.width * frame.height * 4);
		for(int y = 0; y < frame.height; y++)
			for(int x = 0; x < frame.width; x++)
			{
				const size_t at = ((size_t)y * frame.width + x) * 4;
				frame.rgba[at + 0] = (uint8_t)(x * 255 / (frame.width - 1));
				frame.rgba[at + 1] = (uint8_t)(y * 255 / (frame.height - 1));
				frame.rgba[at + 2] = 128;
				frame.rgba[at + 3] = 255;
			}
		const artemis::save::Frame thumb = artemis::save::ScaleStretch(frame, 320, 180);
		Check(thumb.width == 320 && thumb.height == 180, "the thumbnail is the size asked for");
		Check(thumb.rgba.size() == (size_t)320 * 180 * 4, "its buffer is the right size");

		artemis::save::SaveStore store(dir);
		std::string error;
		artemis::save::SaveState state = MakeState(4, false, "with a picture", "少女", 3);
		Check(store.WriteSlot(state, 4, &frame, 320, 180, error), "write a slot with a frame",
			error);
		artemis::save::SaveState back;
		Check(store.ReadSlot(4, back, error), "read it back", error);
		Check(!back.thumbnail.fileName.empty(), "the record says a thumbnail exists",
			back.thumbnail.fileName);
		Check(back.thumbnail.width == 320 && back.thumbnail.height == 180,
			"the record carries the thumbnail size");
		Check(back.thumbnail.sha1.size() == 40, "the record carries the PNG hash");

		artemis::save::Frame fromDisk;
		Check(artemis::save::ReadFramePng(store.ThumbFile(4), fromDisk, error),
			"the thumbnail PNG reads back", error);
		Check(fromDisk.width == 320 && fromDisk.height == 180, "and it is 320x180");
	}

	void TestBacklogRing(const std::string& dir)
	{
		printf("the backlog keeps the last pages, and outlives a slot\n");
		artemis::save::SaveStore store(dir);
		artemis::save::Backlog ring;
		const size_t cap = ring.capacity();
		for(size_t i = 0; i < cap + 5; i++)
			ring.Push("少女", "line " + std::to_string(i), "", 0, (int64_t)i);
		Check(ring.size() == cap, "the ring holds at most its capacity",
			std::to_string(ring.size()) + " of " + std::to_string(cap));
		Check(ring.entries().back().text == "line " + std::to_string(cap + 4),
			"the newest entry is the last one pushed", ring.entries().back().text);
		std::string error;
		Check(store.SaveBacklog(ring, error), "the backlog persists", error);
		artemis::save::Backlog back;
		Check(store.LoadBacklog(back, error), "and loads again", error);
		Check(back.size() == cap, "with the same size");
		Check(back.entries().back().text == ring.entries().back().text,
			"and the same newest entry");
	}

	void TestReadMarks(const std::string& dir)
	{
		printf("already-read marks are shared across slots\n");
		artemis::save::SaveStore store(dir);
		artemis::save::AlreadyRead marks;
		marks.Mark("scenario/scene.iet:tanabata", 1000);
		marks.Mark("scenario/scene.iet:tanabata", 2000);
		marks.Mark("scenario/scene.iet:start", 1500);
		Check(marks.Count("scenario/scene.iet:tanabata") == 2, "a key records how often",
			std::to_string(marks.Count("scenario/scene.iet:tanabata")));
		Check(marks.IsRead("scenario/scene.iet:start"), "another key is read");
		Check(!marks.IsRead("scenario/scene.iet:nowhere"), "an unseen key is not read");
		std::string error;
		Check(store.SaveReadMarks(marks, error), "the marks persist", error);
		artemis::save::AlreadyRead back;
		Check(store.LoadReadMarks(back, error), "and load again", error);
		Check(back.size() == marks.size(), "with the same number of keys");
		Check(back.Count("scenario/scene.iet:tanabata") == 2, "and the same counts");
	}

	void TestStrictness(const std::string& dir)
	{
		printf("junk is refused rather than half-read\n");
		artemis::save::SaveStore store(dir);
		const std::string junkPath = dir + "/junk.dat";
		FILE* f = fopen(junkPath.c_str(), "wb");
		if(f != nullptr)
		{
			fputs("this is not a save file at all", f);
			fclose(f);
		}
		artemis::save::SaveState state;
		std::string error;
		Check(!store.ReadSlot(99, state, error), "reading a slot that does not exist fails");
		Check(!error.empty(), "and says why", error.substr(0, 40));
		remove(junkPath.c_str());
	}

	int SelfTest()
	{
		// Every check runs in a fresh directory: a leftover file from a previous
		// run is exactly the kind of thing that turns a red check green.
		char pattern[] = "/tmp/savetool-XXXXXX";
		const char* dir = mkdtemp(pattern);
		if(dir == nullptr)
			return Fail("cannot make a scratch directory");
		const std::string root = dir;

		printf("savetool selftest in %s\n", root.c_str());
		TestRoundTrip(root);
		TestAutosaveIsSeparate(root);
		TestThumbnail(root);
		TestBacklogRing(root);
		TestReadMarks(root);
		TestStrictness(root);

#ifdef ARTEMIS_SAVE_VM
		{
			const std::string data = std::string(ARTEMIS_TESTDATA)
				+ "/artemis-nicolan/extracted";
			FILE* probe = fopen((data + "/scenario/scene.iet").c_str(), "rb");
			if(probe != nullptr)
			{
				fclose(probe);
				printf("a save resumes the same pages in a new interpreter\n");
				VmEndToEnd(data, 6);
			}
			else
				printf("  resume check SKIPPED: %s is not in this checkout\n", data.c_str());
		}
#else
		printf("  resume check SKIPPED: savetool was built without the interpreter\n");
#endif

		printf("\n%s\n", failures == 0 ? "RESULT: OK" : "RESULT: FAILED");
		return failures == 0 ? 0 : 1;
	}
}

int main(int argc, char** argv)
{
	if(argc < 2)
		return Fail("usage: savetool <save|autosave|load|list|erase|backlog|marks|selftest> …");

	const std::string command = argv[1];
	if(command == "selftest")
		return SelfTest();
	if(argc < 3)
		return Fail("savetool " + command + " needs a save directory");

	const std::string dir = argv[2];
	int slot = -1;
	bool autosave = false;
	std::string framePath;
	int thumbW = 320, thumbH = 180;
	std::string text = "saved line";
	std::string speaker = "";
	std::vector<std::string> vars;
	std::vector<std::string> readKeys;
	std::vector<std::string> pushes;
	bool dump = false;

	for(int i = 3; i < argc; i++)
	{
		const std::string a = argv[i];
		auto next = [&](const char* what) -> std::string {
			if(i + 1 >= argc)
				return std::string();
			(void)what;
			return argv[++i];
		};
		if(a == "--slot")
		{
			const std::string v = next("slot");
			if(v.empty())
				return Fail("--slot wants a number");
			slot = atoi(v.c_str());
		}
		else if(a == "--autosave") autosave = true;
		else if(a == "--frame") framePath = next("frame");
		else if(a == "--text") text = next("text");
		else if(a == "--speaker") speaker = next("speaker");
		else if(a == "--var") vars.push_back(next("var"));
		else if(a == "--read-key") readKeys.push_back(next("read-key"));
		else if(a == "--push") pushes.push_back(next("push"));
		else if(a == "--dump") dump = true;
		else if(a == "--thumb")
		{
			const std::string v = next("thumb");
			if(sscanf(v.c_str(), "%dx%d", &thumbW, &thumbH) != 2)
				return Fail("--thumb wants WxH");
		}
		else
			return Fail("unknown option " + a);
	}

	if(command == "list")
		return List(dir);
	if(command == "vm-e2e")
	{
#ifdef ARTEMIS_SAVE_VM
		printf("resume check on %s\n", dir.c_str());
		VmEndToEnd(dir, 6);
		printf("\n%s\n", failures == 0 ? "RESULT: OK" : "RESULT: FAILED");
		return failures == 0 ? 0 : 1;
#else
		printf("vm-e2e is not built in: this savetool has no interpreter linked\n");
		return 0;
#endif
	}
	if(command == "load")
	{
		if(slot < 0 && !autosave)
			return Fail("load needs --slot N or --autosave");
		return Dump(dir, slot, autosave);
	}
	if(command == "save" || command == "autosave")
	{
		if(command == "save" && slot < 0)
			return Fail("save needs --slot N");
		return Save(dir, slot, command == "autosave" || autosave, framePath, thumbW, thumbH,
			text, speaker, vars, readKeys);
	}
	if(command == "erase")
	{
		artemis::save::SaveStore store(dir);
		if(!store.ok())
			return Fail(store.error());
		std::string error;
		const bool ok = autosave ? store.EraseAutosave(error) : store.EraseSlot(slot, error);
		if(!ok)
			return Fail(error);
		printf("erased %s\n", autosave ? "the autosave" : "slot");
		return 0;
	}
	if(command == "backlog")
	{
		artemis::save::SaveStore store(dir);
		if(!store.ok())
			return Fail(store.error());
		artemis::save::Backlog ring;
		std::string error;
		store.LoadBacklog(ring, error); // an absent file is not an error
		for(const std::string& p : pushes)
		{
			const size_t at = p.find('|');
			ring.Push(at == std::string::npos ? "" : p.substr(0, at),
				at == std::string::npos ? p : p.substr(at + 1), "", 0, NowMs());
		}
		if(!pushes.empty() && !store.SaveBacklog(ring, error))
			return Fail(error);
		if(dump || pushes.empty())
			for(size_t i = 0; i < ring.size(); i++)
				printf("  [%zu] %s: %s\n", i, ring.entries()[i].speaker.c_str(),
					ring.entries()[i].text.c_str());
		return 0;
	}
	if(command == "marks")
	{
		artemis::save::SaveStore store(dir);
		if(!store.ok())
			return Fail(store.error());
		artemis::save::AlreadyRead marks;
		std::string error;
		store.LoadReadMarks(marks, error);
		for(const std::string& k : readKeys)
			marks.Mark(k, NowMs());
		if(!readKeys.empty() && !store.SaveReadMarks(marks, error))
			return Fail(error);
		for(const std::string& k : marks.keys())
			printf("  %s  read %d time(s)\n", k.c_str(), marks.Count(k));
		return 0;
	}
	return Fail("unknown command " + command);
}
