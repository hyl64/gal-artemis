// state.cpp - JSON for every state type, and the real-data semantics check

#include "state.h"

#include "../asb.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>

namespace artemis
{
	namespace save
	{
		const char kMagic[8] = { 'A', 'R', 'T', 'S', 'A', 'V', 'E', '1' };
		const uint32_t kVersion = 1;
		const char* SaveState::kNoData = "--/--/-- --:--";

		// ---------------------------------------------------------------- JSON

		Json BacklogEntry::ToJson() const
		{
			Json out = Json::MakeObject();
			out.Set("speaker", Json::MakeString(speaker));
			out.Set("text", Json::MakeString(text));
			out.Set("file", Json::MakeString(file));
			out.Set("line", Json::MakeNumber(line));
			out.Set("atMs", Json::MakeNumber((double)atMs));
			return out;
		}

		BacklogEntry BacklogEntry::FromJson(const Json& json)
		{
			BacklogEntry out;
			out.speaker = json.stringOr("speaker", "");
			out.text = json.stringOr("text", "");
			out.file = json.stringOr("file", "");
			out.line = (int)json.numberOr("line", 0);
			out.atMs = (int64_t)json.numberOr("atMs", 0);
			return out;
		}

		Json LayerState::ToJson() const
		{
			Json out = Json::MakeObject();
			out.Set("id", Json::MakeString(id));
			out.Set("file", Json::MakeString(file));
			out.Set("visible", Json::MakeBool(visible));
			out.Set("left", Json::MakeNumber(left));
			out.Set("top", Json::MakeNumber(top));
			out.Set("width", Json::MakeNumber(width));
			out.Set("height", Json::MakeNumber(height));
			out.Set("xscale", Json::MakeNumber(xscale));
			out.Set("yscale", Json::MakeNumber(yscale));
			out.Set("alpha", Json::MakeNumber(alpha));
			Json colorJson = Json::MakeArray();
			for(int i = 0; i < 4; i++)
				colorJson.Push(Json::MakeNumber(color[i]));
			out.Set("color", std::move(colorJson));
			out.Set("eventFile", Json::MakeString(eventFile));
			out.Set("eventLabel", Json::MakeString(eventLabel));
			out.Set("slot", Json::MakeNumber(slot));
			return out;
		}

		LayerState LayerState::FromJson(const Json& json)
		{
			LayerState out;
			out.id = json.stringOr("id", "");
			out.file = json.stringOr("file", "");
			out.visible = json.boolOr("visible", true);
			out.left = (int)json.numberOr("left", 0);
			out.top = (int)json.numberOr("top", 0);
			out.width = (int)json.numberOr("width", 0);
			out.height = (int)json.numberOr("height", 0);
			out.xscale = (int)json.numberOr("xscale", 100);
			out.yscale = (int)json.numberOr("yscale", 100);
			out.alpha = (int)json.numberOr("alpha", 255);
			if(const Json* color = json.field("color"))
				if(color->isArray())
					for(size_t i = 0; i < 4 && i < color->items.size(); i++)
						out.color[i] = (uint8_t)color->items[i].number;
			out.eventFile = json.stringOr("eventFile", "");
			out.eventLabel = json.stringOr("eventLabel", "");
			out.slot = (int)json.numberOr("slot", -1);
			return out;
		}

		Json BgmState::ToJson() const
		{
			Json out = Json::MakeObject();
			out.Set("file", Json::MakeString(file));
			out.Set("path", Json::MakeString(path));
			out.Set("playing", Json::MakeBool(playing));
			out.Set("positionMs", Json::MakeNumber(positionMs));
			out.Set("loop", Json::MakeNumber(loop));
			out.Set("volume", Json::MakeNumber(volume));
			return out;
		}

		BgmState BgmState::FromJson(const Json& json)
		{
			BgmState out;
			out.file = json.stringOr("file", "");
			out.path = json.stringOr("path", "");
			out.playing = json.boolOr("playing", false);
			out.positionMs = (int)json.numberOr("positionMs", 0);
			out.loop = (int)json.numberOr("loop", -1);
			out.volume = json.numberOr("volume", 1.0);
			return out;
		}

		Json ThumbnailInfo::ToJson() const
		{
			Json out = Json::MakeObject();
			out.Set("file", Json::MakeString(fileName));
			out.Set("width", Json::MakeNumber(width));
			out.Set("height", Json::MakeNumber(height));
			out.Set("sha1", Json::MakeString(sha1));
			return out;
		}

		ThumbnailInfo ThumbnailInfo::FromJson(const Json& json)
		{
			ThumbnailInfo out;
			out.fileName = json.stringOr("file", "");
			out.width = (int)json.numberOr("width", 0);
			out.height = (int)json.numberOr("height", 0);
			out.sha1 = json.stringOr("sha1", "");
			return out;
		}

		Json PageFrame::ToJson() const
		{
			Json out = Json::MakeObject();
			out.Set("file", Json::MakeString(file));
			out.Set("label", Json::MakeString(label));
			out.Set("speaker", Json::MakeString(speaker));
			out.Set("text", Json::MakeString(text));
			out.Set("pageIndex", Json::MakeNumber(pageIndex));
			return out;
		}

		PageFrame PageFrame::FromJson(const Json& json)
		{
			PageFrame out;
			out.file = json.stringOr("file", "");
			out.label = json.stringOr("label", "");
			out.speaker = json.stringOr("speaker", "");
			out.text = json.stringOr("text", "");
			out.pageIndex = (int)json.numberOr("pageIndex", 0);
			return out;
		}

		std::string SaveState::FormatTime(int64_t timestampMs)
		{
			const time_t seconds = (time_t)(timestampMs / 1000);
			struct tm local;
			localtime_r(&seconds, &local);
			char buffer[32];
			snprintf(buffer, sizeof(buffer), "%02d/%02d/%02d %02d:%02d",
				(local.tm_year + 1900) % 100, local.tm_mon + 1, local.tm_mday,
				local.tm_hour, local.tm_min);
			return buffer;
		}

		std::string SaveState::Summary() const
		{
			std::string out = autosave ? "autosave" : ("slot " + std::to_string(slot));
			out += "  " + (timestampMs ? FormatTime(timestampMs) : std::string(kNoData));
			if(!scriptFile.empty())
				out += "  " + scriptFile;
			if(!startLabel.empty())
				out += " *" + startLabel;
			out += "  page " + std::to_string(pageIndex);
			if(!speaker.empty())
				out += "  [" + speaker + "]";
			if(!text.empty())
			{
				std::string one = text;
				for(char& c : one)
					if(c == '\n')
						c = ' ';
				if(one.size() > 40)
					one = one.substr(0, 37) + "...";
				out += "  " + one;
			}
			return out;
		}

		Json SaveState::ToJson() const
		{
			Json out = Json::MakeObject();
			out.Set("formatVersion", Json::MakeNumber(formatVersion));
			out.Set("slot", Json::MakeNumber(slot));
			out.Set("autosave", Json::MakeBool(autosave));
			out.Set("timestampMs", Json::MakeNumber((double)timestampMs));
			out.Set("displayTime", Json::MakeString(displayTime));
			out.Set("gameId", Json::MakeString(gameId));
			out.Set("engineVersion", Json::MakeString(engineVersion));
			out.Set("scriptFile", Json::MakeString(scriptFile));
			out.Set("startLabel", Json::MakeString(startLabel));
			out.Set("block", Json::MakeString(block));
			out.Set("pageIndex", Json::MakeNumber(pageIndex));
			out.Set("speaker", Json::MakeString(speaker));
			out.Set("text", Json::MakeString(text));

			Json vars = Json::MakeObject();
			for(const auto& v : this->vars)
				vars.Set(v.first, Json::MakeString(v.second));
			out.Set("vars", std::move(vars));

			Json layers = Json::MakeArray();
			for(const LayerState& layer : this->layers)
				layers.Push(layer.ToJson());
			out.Set("layers", std::move(layers));

			out.Set("bgm", bgm.ToJson());

			Json read = Json::MakeArray();
			for(const std::string& key : readKeys)
				read.Push(Json::MakeString(key));
			out.Set("readKeys", std::move(read));

			Json backlog = Json::MakeArray();
			for(const BacklogEntry& entry : this->backlog)
				backlog.Push(entry.ToJson());
			out.Set("backlog", std::move(backlog));

			out.Set("thumbnail", thumbnail.ToJson());
			return out;
		}

		SaveState SaveState::FromJson(const Json& json)
		{
			SaveState out;
			out.formatVersion = (uint32_t)json.numberOr("formatVersion", kVersion);
			out.slot = (int)json.numberOr("slot", 0);
			out.autosave = json.boolOr("autosave", false);
			out.timestampMs = (int64_t)json.numberOr("timestampMs", 0);
			out.displayTime = json.stringOr("displayTime", "");
			out.gameId = json.stringOr("gameId", "");
			out.engineVersion = json.stringOr("engineVersion", "");
			out.scriptFile = json.stringOr("scriptFile", "");
			out.startLabel = json.stringOr("startLabel", "");
			out.block = json.stringOr("block", "");
			out.pageIndex = (int)json.numberOr("pageIndex", 0);
			out.speaker = json.stringOr("speaker", "");
			out.text = json.stringOr("text", "");
			if(const Json* vars = json.field("vars"))
				if(vars->isObject())
					for(const auto& v : vars->fields)
						out.vars[v.first] = v.second.kind == Json::Kind::String
							? v.second.text : v.second.Dump(0);
			if(const Json* layers = json.field("layers"))
				if(layers->isArray())
					for(const Json& layer : layers->items)
						out.layers.push_back(LayerState::FromJson(layer));
			if(const Json* bgm = json.field("bgm"))
				out.bgm = BgmState::FromJson(*bgm);
			if(const Json* read = json.field("readKeys"))
				if(read->isArray())
					for(const Json& key : read->items)
						out.readKeys.push_back(key.text);
			if(const Json* backlog = json.field("backlog"))
				if(backlog->isArray())
					for(const Json& entry : backlog->items)
						out.backlog.push_back(BacklogEntry::FromJson(entry));
			if(const Json* thumb = json.field("thumbnail"))
				out.thumbnail = ThumbnailInfo::FromJson(*thumb);
			return out;
		}

		// ------------------------------------------------------------ semantics

		SaveSemantics DefaultSemantics()
		{
			SaveSemantics s;
			s.slotFiles = { "save<N>.dat" };
			s.thumbFiles = { "save<N>.png" };
			s.autosaveFile = "autosave.dat";
			s.autosaveSlotVar = "t.autosave_slot";
			s.writeOrder = { "saving=1", "calllua sv.save", "se_saveok", "return" };
			s.modeVars = { "t.saving", "t.sldata" };
			s.timeFormat = "yy/MM/dd hh:mm";
			s.noExistText = "--/--/-- --:--";
			s.thumbSizeVars = "t.savethum_width/t.savethum_height";
			return s;
		}

		bool VerifySemanticsAgainstRealData(const std::string& testdataRoot,
			std::vector<std::string>& problems, bool& found)
		{
			problems.clear();
			found = false;
			const std::string macro = testdataRoot
				+ "/artemis-nicolan/extracted/system/save_normal.iet";
			const std::string asbPath = testdataRoot
				+ "/artemis-raspberry-web/samples/asb/system_save.asb";

			struct stat st;
			if(stat(macro.c_str(), &st) == 0)
			{
				found = true;
				FILE* f = fopen(macro.c_str(), "rb");
				std::string bytes;
				if(f)
				{
					std::vector<char> buffer((size_t)st.st_size);
					const size_t got = fread(buffer.data(), 1, buffer.size(), f);
					bytes.assign(buffer.data(), got);
					fclose(f);
				}
				// the macro's own spellings, straight out of the file
				const char* wanted[] = { ".dat", ".png", "autosave.dat", "t.autosave_slot",
					"t.saving", "t.sldata", "yy/MM/dd hh:mm", "--/--/-- --:--",
					"t.savethum_width", "file_exist", "file_update_time" };
				for(const char* needle : wanted)
					if(bytes.find(needle) == std::string::npos)
						problems.push_back(std::string("save_normal.iet does not mention ")
							+ needle);
			}

			if(stat(asbPath.c_str(), &st) == 0)
			{
				found = true;
				FILE* f = fopen(asbPath.c_str(), "rb");
				std::vector<uint8_t> bytes;
				if(f)
				{
					bytes.resize((size_t)st.st_size);
					bytes.resize(fread(bytes.data(), 1, bytes.size(), f));
					fclose(f);
				}
				Asb asb;
				std::string error;
				if(!ParseAsb(bytes.data(), bytes.size(), asb, error))
					problems.push_back("system_save.asb does not parse: " + error);
				else
				{
					// the write order the bytecode shows, checked as a subsequence
					std::vector<std::string> names;
					for(const AsbItem& item : asb.items)
						if(item.kind == AsbItem::Kind::Command)
							names.push_back(item.name);
					const char* order[] = { "saving", "calllua", "se_saveok", "return" };
					size_t at = 0;
					for(const char* want : order)
					{
						bool hit = false;
						for(; at < names.size(); at++)
							if(names[at] == want)
							{
								hit = true;
								at++;
								break;
							}
						if(!hit)
							problems.push_back(std::string("system_save.asb: '") + want
								+ "' is not in the expected order");
					}
					if(asb.truncated)
						problems.push_back("system_save.asb did not decode to the end: " + asb.note);
				}
			}
			return problems.empty();
		}
	}
}