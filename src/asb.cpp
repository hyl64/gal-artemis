// asb.cpp - the .asb bytecode reader described in asb.h

#include "asb.h"

#include <cstring>

namespace artemis
{
	namespace
	{
		// In addition to this header guard, named `Reader` inside a namespace, this
		// little cursor checks every read: a real ASB is a file we do not control,
		// so a bad length must stop the walk, not wander memory.
		struct Reader
		{
			const uint8_t* data;
			size_t size;
			size_t at = 0;
			std::string why;

			bool Need(size_t n)
			{
				if(at + n > size)
				{
					why = "ran past the end of the file at offset " + std::to_string(at);
					return false;
				}
				return true;
			}

			bool U32(uint32_t& out)
			{
				if(!Need(4))
					return false;
				out = (uint32_t)data[at] | (uint32_t)data[at + 1] << 8
					| (uint32_t)data[at + 2] << 16 | (uint32_t)data[at + 3] << 24;
				at += 4;
				return true;
			}

			bool I32(int32_t& out)
			{
				uint32_t v = 0;
				if(!U32(v))
					return false;
				out = (int32_t)v;
				return true;
			}

			// u32 length, then that many bytes of UTF-8 including a terminating
			// NUL; the length must agree with the NUL, which is the check that
			// catches a wrong offset immediately
			bool String(std::string& out)
			{
				uint32_t length = 0;
				if(!U32(length))
					return false;
				if(length == 0)
				{
					why = "zero-length string at offset " + std::to_string(at - 4);
					return false;
				}
				// the length does *not* include the terminating NUL: the bytes are
				// `length` of UTF-8 followed by exactly one 0x00, with no padding
				if(!Need((size_t)length + 1))
					return false;
				if(data[at + length] != 0)
				{
					why = "string at offset " + std::to_string(at)
						+ " is not NUL terminated as its length says";
					return false;
				}
				out.assign((const char*)data + at, length);
				at += length + 1;
				return true;
			}
		};
	}

	bool LooksLikeAsb(const uint8_t* data, size_t size)
	{
		return size >= 5 && memcmp(data, "ASB\0\0", 5) == 0;
	}

	bool ParseAsb(const uint8_t* data, size_t size, Asb& out, std::string& error)
	{
		if(!LooksLikeAsb(data, size))
		{
			error = "not an ASB file: the header is not \"ASB\\0\\0\"";
			return false;
		}
		Reader reader{ data, size };
		reader.at = 5;
		if(!reader.U32(out.itemCount))
		{
			error = "the ASB header is cut short: " + reader.why;
			return false;
		}
		if(out.itemCount > 4000000u)
		{
			error = "implausible ASB item count " + std::to_string(out.itemCount);
			return false;
		}
		out.items.reserve(out.itemCount < 4096 ? out.itemCount : 4096);
		for(uint32_t i = 0; i < out.itemCount; i++)
		{
			AsbItem item;
			uint32_t type = 0;
			if(!reader.U32(type))
			{
				out.truncated = true;
				out.note = reader.why;
				break;
			}
			if(type == 0)
			{
				item.kind = AsbItem::Kind::Command;
				int32_t line = 0;
				uint32_t attributes = 0;
				if(!reader.String(item.name) || !reader.I32(line) || !reader.U32(attributes))
				{
					out.truncated = true;
					out.note = reader.why;
					break;
				}
				item.line = line;
				item.attributes.reserve(attributes < 256 ? attributes : 256);
				bool broken = false;
				for(uint32_t a = 0; a < attributes; a++)
				{
					AsbAttribute attribute;
					if(!reader.String(attribute.name) || !reader.String(attribute.value))
					{
						broken = true;
						break;
					}
					item.attributes.push_back(std::move(attribute));
				}
				if(broken)
				{
					out.truncated = true;
					out.note = reader.why;
					break;
				}
			}
			else if(type == 1)
			{
				item.kind = AsbItem::Kind::Label;
				if(!reader.String(item.name))
				{
					out.truncated = true;
					out.note = reader.why;
					break;
				}
			}
			else
			{
				out.truncated = true;
				out.note = "unknown item type " + std::to_string(type) + " at item "
					+ std::to_string(i) + " (offset " + std::to_string(reader.at - 4) + ")";
				break;
			}
			out.items.push_back(std::move(item));
		}
		return true;
	}
}