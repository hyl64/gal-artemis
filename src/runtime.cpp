// runtime.cpp - walking a scenario into actions

#include "runtime.h"

#include <stdexcept>

namespace artemis
{
	Scenario::Scenario(const std::string& source)
	{
		Script script = Parse(source);
		const Node* text = nullptr;
		for(auto& m : script.top)
			if(m.first == "text")
				text = &m.second;
		if(text == nullptr)
			throw std::runtime_error("runtime: the script has no text block");

		// the scenes are the named members of the text block; a script that is
		// one flat list runs as a single unnamed scene
		// the nodes are copied: the parsed script is local to this constructor,
		// and the runtime has to outlive it
		if(text->type == NodeType::Dictionary)
		{
			for(auto& m : text->members)
				scenes.emplace_back(m.first, m.second);
		}
		else
			scenes.emplace_back(std::string(), *text);
		if(!scenes.empty())
			EnterScene(scenes.front().first);
	}

	std::vector<std::string> Scenario::SceneNames() const
	{
		std::vector<std::string> names;
		for(auto& s : scenes)
			names.push_back(s.first);
		return names;
	}

	void Scenario::EnterScene(const std::string& name)
	{
		current = name;
		finished = false;
		for(auto& s : scenes)
		{
			if(s.first != name)
				continue;
			// the scene's own entries become the actions, in order
			const Node& block = s.second;
			// a scene is a list of entries plus a few named ones (goto, wait); the
			// list comes first, then the named ones, which is the order the file
			// reads in
			for(auto& item : block.items)
			{
				if(item.type == NodeType::String)
					pending.push_back({ActionType::Line, item.text, 0});
				else if(item.type == NodeType::Tag)
				{
					if(item.hasTagNumber)
						pending.push_back({ActionType::Wait, std::string(), item.integer});
					else
						pending.push_back({ActionType::Wait, std::string(), 0});
				}
				else if(item.type == NodeType::Dictionary)
				{
					for(auto& m : item.members)
					{
						if(m.first == "goto")
							pending.push_back({ActionType::Jump, m.second.text, 0});
						else if(m.first == "wait")
							pending.push_back({ActionType::Wait, std::string(), m.second.integer});
						else
							pending.push_back({ActionType::Unknown, m.first, 0});
					}
				}
				else
					pending.push_back({ActionType::Unknown, item.text, 0});
			}
			for(auto& m : block.members)
			{
				// a block that mixes list entries with named ones keeps the
				// unnamed entries as members with no name
				if(m.first.empty())
				{
					if(m.second.type == NodeType::String)
						pending.push_back({ActionType::Line, m.second.text, 0});
					else if(m.second.type == NodeType::Tag)
						pending.push_back({ActionType::Wait, std::string(),
							m.second.hasTagNumber ? m.second.integer : 0});
					else
						pending.push_back({ActionType::Unknown, m.second.text, 0});
					continue;
				}
				if(m.first == "goto")
					pending.push_back({ActionType::Jump, m.second.text, 0});
				else if(m.first == "wait")
					pending.push_back({ActionType::Wait, std::string(), m.second.integer});
				else if(m.first != "name")
					pending.push_back({ActionType::Unknown, m.first, 0});
			}
			return;
		}
		finished = true;
	}

	Action Scenario::Next()
	{
		if(finished && pending.empty())
			return Action{ActionType::End, std::string(), 0};
		if(pending.empty())
		{
			finished = true;
			return Action{ActionType::End, std::string(), 0};
		}
		Action a = pending.front();
		pending.pop_front();
		if(a.type == ActionType::Jump)
			EnterScene(a.text);
		return a;
	}
}
