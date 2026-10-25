// readmarks.cpp - the already-read set, as described in readmarks.h

#include "readmarks.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace artemis
{
	namespace save
	{
		const char* AlreadyRead::kHeader = "ARTEMIS-ALREADYREAD 1";

		void AlreadyRead::Mark(const std::string& key, int64_t atMs)
		{
			if(key.empty())
				return;
			ReadMark& mark = m_counts[key];
			mark.count++;
			if(atMs > mark.lastMs)
				mark.lastMs = atMs;
			m_total++;
		}

		bool AlreadyRead::IsRead(const std::string& key) const
		{
			return m_counts.find(key) != m_counts.end();
		}

		int AlreadyRead::Count(const std::string& key) const
		{
			auto it = m_counts.find(key);
			return it == m_counts.end() ? 0 : it->second.count;
		}

		std::vector<std::string> AlreadyRead::keys() const
		{
			std::vector<std::string> out;
			out.reserve(m_counts.size());
			for(const auto& entry : m_counts)
				out.push_back(entry.first);
			return out;
		}

		void AlreadyRead::Clear()
		{
			m_counts.clear();
			m_total = 0;
		}

		bool AlreadyRead::SaveTo(const std::string& path, std::string& error) const
		{
			FILE* f = fopen(path.c_str(), "wb");
			if(f == nullptr)
			{
				error = "cannot write " + path;
				return false;
			}
			fprintf(f, "%s\n", kHeader);
			fprintf(f, "total %lld\n", (long long)m_total);
			// count and time first, the key last, so a key may contain spaces
			for(const auto& entry : m_counts)
				fprintf(f, "%d %lld %s\n", entry.second.count,
					(long long)entry.second.lastMs, entry.first.c_str());
			fclose(f);
			return true;
		}

		bool AlreadyRead::LoadFrom(const std::string& path, std::string& error)
		{
			FILE* f = fopen(path.c_str(), "rb");
			if(f == nullptr)
			{
				error = "no read-marks file at " + path;
				return false;
			}
			char line[4096];
			if(fgets(line, sizeof(line), f) == nullptr)
			{
				fclose(f);
				error = "the read-marks file is empty";
				return false;
			}
			std::string header(line);
			while(!header.empty() && (header.back() == '\n' || header.back() == '\r'))
				header.pop_back();
			if(header != kHeader)
			{
				fclose(f);
				error = "not a read-marks file (header is \"" + header + "\")";
				return false;
			}
			AlreadyRead loaded;
			while(fgets(line, sizeof(line), f) != nullptr)
			{
				std::string text(line);
				while(!text.empty() && (text.back() == '\n' || text.back() == '\r'))
					text.pop_back();
				if(text.empty())
					continue;
				if(text.rfind("total ", 0) == 0)
				{
					loaded.m_total = strtoll(text.c_str() + 6, nullptr, 10);
					continue;
				}
				const size_t firstSpace = text.find(' ');
				if(firstSpace == std::string::npos)
					continue;
				const size_t secondSpace = text.find(' ', firstSpace + 1);
				if(secondSpace == std::string::npos)
					continue;
				ReadMark mark;
				mark.count = atoi(text.substr(0, firstSpace).c_str());
				mark.lastMs = strtoll(text.substr(firstSpace + 1,
					secondSpace - firstSpace - 1).c_str(), nullptr, 10);
				const std::string key = text.substr(secondSpace + 1);
				if(!key.empty())
					loaded.m_counts[key] = mark;
			}
			fclose(f);
			m_counts = loaded.m_counts;
			m_total = loaded.m_total;
			return true;
		}
	}
}