// readmarks.h - "already read", across every slot
//
// The real save screen shows which scenes a player has already seen (that is what
// makes a skip button legal), so the marks cannot live inside one save file: they
// have to outlive a slot being overwritten, and be shared by autosave and the
// numbered slots alike.  Hence one file of their own:
//
//   alreadyread.dat   a set of keys plus how often each was read
//
// A key is "script:label" - the finest unit the interpreter knows - and the count
// is kept because "read once" and "read twice" are different questions for a
// replay counter.

#ifndef ARTEMIS_SAVE_READMARKS_H
#define ARTEMIS_SAVE_READMARKS_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace artemis
{
	namespace save
	{
		class AlreadyRead
		{
		public:
			// Mark a key (or increment its count) and set its last-seen time.
			void Mark(const std::string& key, int64_t atMs = 0);
			bool IsRead(const std::string& key) const;
			int Count(const std::string& key) const;
			size_t size() const { return m_counts.size(); }
			int64_t totalReads() const { return m_total; }

			std::vector<std::string> keys() const; // sorted, stable output
			void Clear();

			bool SaveTo(const std::string& path, std::string& error) const;
			bool LoadFrom(const std::string& path, std::string& error);

			// "alreadyread.dat" holds star-separated records; the loader is strict
			// about the header so an unrelated file cannot be mistaken for marks
			static const char* kHeader;

		private:
			// Not called `Mark`: the public method of that name would hide it.
			struct ReadMark
			{
				int count = 0;
				int64_t lastMs = 0;
			};
			std::map<std::string, ReadMark> m_counts;
			int64_t m_total = 0;
		};
	}
}

#endif // ARTEMIS_SAVE_READMARKS_H