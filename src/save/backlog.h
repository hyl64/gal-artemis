// backlog.h - the ring of pages the reader has already seen
//
// The real title has `[backlog]` and `[writebacklog]`: a scrolling history of the
// last N pages, with the speaker for each.  It is a *ring* on purpose - memory
// does not grow with the script - and the save screen wants a copy of it, so it
// is serializable both into a save and into its own file.
//
// The capacity is a policy, not a format constant: the game's own number is not
// legible in the extracted macro, so this tree takes 100 (and says so in
// docs/SAVE.md) and the CLI can change it per run.

#ifndef ARTEMIS_SAVE_BACKLOG_H
#define ARTEMIS_SAVE_BACKLOG_H

#include "state.h"

#include <cstddef>
#include <string>
#include <vector>

namespace artemis
{
	namespace save
	{
		class Backlog
		{
		public:
			explicit Backlog(size_t capacity = 100);

			size_t capacity() const { return m_capacity; }
			void SetCapacity(size_t capacity); // keeps the newest entries

			void Push(const BacklogEntry& entry);
			void Push(const std::string& speaker, const std::string& text,
				const std::string& file, int line, int64_t atMs);

			const std::vector<BacklogEntry>& entries() const { return m_entries; }
			size_t size() const { return m_entries.size(); }
			int64_t totalPushed() const { return m_total; }
			void Clear();

			// oldest first, which is how a backlog panel reads
			std::string Dump(size_t limit = 0) const;

			// the ring as bytes for a save file / its own file
			bool SaveTo(const std::string& path, std::string& error) const;
			bool LoadFrom(const std::string& path, std::string& error);

		private:
			std::vector<BacklogEntry> m_entries; // oldest first, capacity at most
			size_t m_capacity;
			int64_t m_total = 0; // how many pages were ever pushed
		};
	}
}

#endif // ARTEMIS_SAVE_BACKLOG_H