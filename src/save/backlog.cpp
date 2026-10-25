// backlog.cpp - the ring described in backlog.h
//
// Its own file (`backlog.dat`) uses the same sectioned envelope as a save, so a
// corrupt ring fails at read time instead of yielding wrong history.

#include "backlog.h"
#include "format.h"

#include <cstdio>

namespace artemis
{
	namespace save
	{
		Backlog::Backlog(size_t capacity) : m_capacity(capacity ? capacity : 1) {}

		void Backlog::SetCapacity(size_t capacity)
		{
			m_capacity = capacity ? capacity : 1;
			while(m_entries.size() > m_capacity)
				m_entries.erase(m_entries.begin());
		}

		void Backlog::Push(const BacklogEntry& entry)
		{
			m_entries.push_back(entry);
			m_total++;
			while(m_entries.size() > m_capacity)
				m_entries.erase(m_entries.begin());
		}

		void Backlog::Push(const std::string& speaker, const std::string& text,
			const std::string& file, int line, int64_t atMs)
		{
			BacklogEntry entry;
			entry.speaker = speaker;
			entry.text = text;
			entry.file = file;
			entry.line = line;
			entry.atMs = atMs;
			Push(entry);
		}

		void Backlog::Clear()
		{
			m_entries.clear();
			m_total = 0;
		}

		std::string Backlog::Dump(size_t limit) const
		{
			std::string out;
			const size_t start = (limit > 0 && limit < m_entries.size())
				? m_entries.size() - limit : 0;
			for(size_t i = start; i < m_entries.size(); i++)
			{
				const BacklogEntry& entry = m_entries[i];
				out += std::to_string(i + 1) + ". ";
				if(!entry.speaker.empty())
					out += "[" + entry.speaker + "] ";
				out += entry.text;
				if(!entry.file.empty())
					out += "   (" + entry.file + ":" + std::to_string(entry.line) + ")";
				out += "\n";
			}
			return out;
		}

		// The file is a save envelope with a single BLCK section, so the CRC and
		// the length checks are the same code path as a save.
		bool Backlog::SaveTo(const std::string& path, std::string& error) const
		{
			SaveState holder; // only used as a container for the serializer
			holder.backlog = m_entries;
			const std::vector<uint8_t> bytes = SerializeState(holder);
			FILE* f = fopen(path.c_str(), "wb");
			if(f == nullptr)
			{
				error = "cannot write " + path;
				return false;
			}
			const size_t wrote = bytes.empty() ? 0
				: fwrite(bytes.data(), 1, bytes.size(), f);
			fclose(f);
			if(wrote != bytes.size())
			{
				error = "short write to " + path;
				return false;
			}
			return true;
		}

		bool Backlog::LoadFrom(const std::string& path, std::string& error)
		{
			FILE* f = fopen(path.c_str(), "rb");
			if(f == nullptr)
			{
				error = "no backlog file at " + path;
				return false;
			}
			fseek(f, 0, SEEK_END);
			const long size = ftell(f);
			fseek(f, 0, SEEK_SET);
			std::vector<uint8_t> bytes((size_t)(size > 0 ? size : 0));
			if(size > 0)
				bytes.resize(fread(bytes.data(), 1, bytes.size(), f));
			fclose(f);
			SaveState holder;
			if(!DeserializeState(bytes.data(), bytes.size(), holder, error))
				return false;
			m_entries = holder.backlog;
			while(m_entries.size() > m_capacity)
				m_entries.erase(m_entries.begin());
			m_total = (int64_t)m_entries.size();
			return true;
		}
	}
}