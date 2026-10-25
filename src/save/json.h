// json.h - the little JSON reader/writer the save layer needs
//
// Two jobs, both small: `savetool save --from-state <file.json>` has to read a
// state a front end dumped, and `savetool load --json` has to print a stored
// state for a person or a script to read.  Artemis itself never stores JSON - the
// save files are the binary format in format.h - so this is deliberately not a
// general JSON library: objects, arrays, strings, numbers, booleans and null,
// no comments, no duplicate-key policy beyond last-wins, and a hard error on
// anything malformed.

#ifndef ARTEMIS_SAVE_JSON_H
#define ARTEMIS_SAVE_JSON_H

#include <string>
#include <utility>
#include <vector>

namespace artemis
{
	namespace save
	{
		struct Json
		{
			enum class Kind
			{
				Null,
				Bool,
				Number,
				String,
				Array,
				Object,
			};

			Kind kind = Kind::Null;
			bool boolean = false;
			double number = 0;
			std::string text; // String contents, or a Number's original spelling
			std::vector<Json> items;
			std::vector<std::pair<std::string, Json>> fields;

			bool isObject() const { return kind == Kind::Object; }
			bool isArray() const { return kind == Kind::Array; }
			bool isString() const { return kind == Kind::String; }
			bool isNumber() const { return kind == Kind::Number; }

			const Json* field(const std::string& name) const; // last write wins
			std::string stringOr(const std::string& name, const std::string& fallback) const;
			double numberOr(const std::string& name, double fallback) const;
			bool boolOr(const std::string& name, bool fallback) const;

			// text is the string; number is printed as written; an object or array
			// is emitted compactly, which is enough for a diagnostics sidecar
			static Json MakeString(const std::string& value);
			static Json MakeNumber(double value);
			static Json MakeBool(bool value);
			static Json MakeObject();
			static Json MakeArray();
			void Set(const std::string& name, Json value);
			void Push(Json value);

			std::string Dump(int indent = 2) const;
		};

		// Parse; on failure `error` says where and why.
		bool ParseJson(const std::string& text, Json& out, std::string& error);
	}
}

#endif // ARTEMIS_SAVE_JSON_H