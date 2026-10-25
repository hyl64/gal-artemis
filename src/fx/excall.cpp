// excall.cpp - see excall.h

#include "fx/excall.h"

#include <map>
#include <memory>
#include <set>

namespace artemis
{
namespace fx
{
	namespace
	{
		constexpr int kMaxCallDepth = 64;
		constexpr size_t kMaxEvents = 50000;

		class Walker
		{
		public:
			Walker(const ScriptLoader& loader, size_t commandLimit)
				: m_loader(loader), m_limit(commandLimit) {}

			ExcallWalk stats;

			void Run(std::string file, std::string label, int depth)
			{
				if(stats.limitHit)
					return;
				if(depth > kMaxCallDepth)
				{
					stats.cycles++;
					Event("depth-limit:" + file);
					return;
				}
				std::shared_ptr<ScriptProgram> program;
				if(!Load(file, program))
				{
					stats.unresolved++;
					if(m_unresolved.insert(file).second)
						stats.unresolvedFiles.push_back(file);
					Event("unresolved:" + file);
					return;
				}
				const std::string start = program->ResolveLabel(label);
				if(start.empty())
				{
					stats.missingLabels++;
					Event("missing-label:" + file + ":" + (label.empty() ? "top" : label));
					return;
				}
				m_active.push_back(file);
				struct Guard
				{
					std::vector<std::string>& active;
					~Guard() { active.pop_back(); }
				} guard{ m_active };

				std::string blockName = start;
				std::set<std::string> visited;
				while(!blockName.empty() && !stats.limitHit)
				{
					if(!visited.insert(blockName).second)
					{
						stats.cycles++; // a linknext loop inside one script
						Event("link-loop:" + file + ":" + blockName);
						break;
					}
					const ScriptBlock* block = program->Block(blockName);
					if(block == nullptr)
					{
						stats.missingLabels++;
						Event("missing-block:" + file + ":" + blockName);
						break;
					}
					stats.blocks++;
					bool returned = false;
					for(const ScriptCommand& command : block->commands)
					{
						stats.commands++;
						if(stats.commands > (int)m_limit)
						{
							stats.limitHit = true;
							Event("limit");
							return;
						}
						if(command.name == "excall")
						{
							const std::string target = command.Attr("file");
							std::string targetLabel = command.Attr("label");
							if(targetLabel.empty())
								targetLabel = "top";
							if(target.empty())
							{
								stats.badCalls++;
								Event("bad-excall");
								continue;
							}
							stats.calls++;
							if(depth + 1 > stats.maxDepth)
								stats.maxDepth = depth + 1;
							Event("call:" + target + ":" + targetLabel);
							if(Active(target))
							{
								stats.cycles++; // a script that calls itself
								Event("cycle:" + target);
								continue;
							}
							Run(target, targetLabel, depth + 1);
							if(stats.limitHit)
								return;
							stats.returns++;
							Event("return:" + target);
						}
						else if(command.name == "return" || command.name == "excallend")
						{
							stats.returnCommands++;
							Event("return-command");
							returned = true;
							break;
						}
						else
							Event(command.name);
					}
					if(returned)
						break;
					blockName = block->next;
				}
			}

		private:
			bool Load(const std::string& file, std::shared_ptr<ScriptProgram>& out)
			{
				const std::map<std::string, std::shared_ptr<ScriptProgram>>::iterator found =
					m_cache.find(file);
				if(found != m_cache.end())
				{
					out = found->second;
					return out != nullptr;
				}
				ScriptProgram program;
				std::string error;
				if(!m_loader(file, program, error))
				{
					m_cache[file] = nullptr;
					stats.unresolvedReasons.push_back(file + ": " + error);
					return false;
				}
				program.file = file;
				out = std::make_shared<ScriptProgram>(std::move(program));
				m_cache[file] = out;
				stats.filesLoaded++;
				return true;
			}

			bool Active(const std::string& file) const
			{
				for(const std::string& active : m_active)
					if(active == file)
						return true;
				return false;
			}

			void Event(const std::string& event)
			{
				if(stats.events.size() < kMaxEvents)
					stats.events.push_back(event);
			}

			const ScriptLoader& m_loader;
			size_t m_limit;
			std::map<std::string, std::shared_ptr<ScriptProgram>> m_cache;
			std::vector<std::string> m_active;
			std::set<std::string> m_unresolved;
		};
	}

	std::string ScriptCommand::Attr(const std::string& name,
		const std::string& fallback) const
	{
		for(const AttrNameValue& attr : attrs)
			if(attr.name == name)
				return attr.value;
		return fallback;
	}

	const ScriptBlock* ScriptProgram::Block(const std::string& name) const
	{
		for(const ScriptBlock& block : blocks)
			if(block.name == name)
				return &block;
		return nullptr;
	}

	std::string ScriptProgram::ResolveLabel(const std::string& label) const
	{
		const std::string wanted = label.empty() ? std::string("top") : label;
		for(const std::pair<std::string, std::string>& entry : labels)
			if(entry.first == wanted)
				return entry.second;
		if(wanted == "top" && Block("block_00000") != nullptr)
			return "block_00000"; // the label table is optional, block_00000 is not
		return std::string();
	}

	ExcallWalk WalkExcall(const std::string& file, const std::string& label,
		const ScriptLoader& loader, size_t commandLimit)
	{
		Walker walker(loader, commandLimit);
		walker.Run(file, label, 0);
		return walker.stats;
	}
}
} // namespace artemis::fx
