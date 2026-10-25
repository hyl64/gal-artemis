// fuzz.cpp - the four parsers against bad data
//
// The engine ships seven tools and four parsers, and every one of their self
// tests feeds them something a developer wrote.  The files a player actually
// loads are not like that: the corpus already contains `[lytween]` tags with no
// destination, `[debug mode=1" level="1"]` with a misplaced quote and a macro
// that says `msec=` where the tag wants `time=`.  A compatibility layer that
// dies on the eighth page of a real game is a compatibility layer that does not
// load the game, so "bad input never kills the process" is a shipping
// requirement, not a nicety.
//
// This program is that requirement, executed.  It takes real seeds, mutates
// them the way damaged data actually arrives (a truncated download, a field
// multiplied by ten, a Shift-JIS trail byte lost, a quote one character off),
// and runs each input in its own process.  An input is acceptable when the
// parser either returns a result or refuses with a reason.  Anything else is a
// finding:
//
//   crash          SIGSEGV/SIGABRT/SIGBUS: the process died
//   timeout        the parse did not finish within the deadline
//   oom            the parse asked for more memory than a real machine may give
//   silent-reject  refused, but with an empty explanation (the caller cannot
//                  tell this apart from a bug in its own code)
//   error-success  claimed success, then produced a result that contradicts the
//                  input (an .asb that says 400 items and yields 3, an archive
//                  whose entry runs past the end of the file)
//   bad-exception  let an exception type escape that a caller cannot expect:
//                  std::out_of_range from a vector::at, std::length_error from
//                  an unchecked resize, so the "clean failure" path is bypassed
//
// Each finding is saved with a minimised reproduction and the command to run it.
//
// Usage:
//   fuzz <pfs|ast|iet|ipt|asb> --inputs 5000 [--rng 1] [--timeout 2000]
//        [--memcap 512] [--corpus <testdata>] [--out <dir>] [--stop-after 20]
//   fuzz limits [--corpus <testdata>]     resource-limit probes
//   fuzz repro <file> <pfs|ast|iet|ipt|asb>  re-run one input, print the verdict
//   fuzz all --inputs 5000                every parser in turn
//
// It compiles against the sources, not the build directory, so it can never
// test a stale library (macOS make compares mtimes in whole seconds, and a
// rebuilt .a inside the same second is silently "up to date").

#include "asb.h"
#include "iet.h"
#include "ipt.h"
#include "luaast.h"
#include "pfs.h"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <typeinfo>
#include <random>
#include <string>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
	// ---------------------------------------------------------------------------
	// verdicts
	// ---------------------------------------------------------------------------
	enum VerdictCode
	{
		kOk = 0,           // parsed, and the result holds together
		kRejected = 1,     // refused with a reason: a good outcome for bad data
		kSilentReject = 3, // refused with no reason
		kErrorAsSuccess = 4,
		kBadException = 5,
		kOom = 6,
		kAsanExit = 42, // ASAN_OPTIONS=exitcode, so a sanitizer stop is not "rejected"
	};

	const char* VerdictName(int code)
	{
		switch(code)
		{
		case kOk: return "ok";
		case kRejected: return "rejected";
		case kSilentReject: return "silent-reject";
		case kErrorAsSuccess: return "error-success";
		case kBadException: return "bad-exception";
		case kOom: return "oom";
		default: return "unknown";
		}
	}

	struct Verdict
	{
		int code = kOk;
		std::string detail;
	};

	Verdict Good() { return Verdict(); }

	Verdict Rejected(const std::string& why)
	{
		Verdict v;
		v.code = kRejected;
		v.detail = why;
		return v;
	}

	Verdict SilentReject()
	{
		Verdict v;
		v.code = kSilentReject;
		v.detail = "Parse returned false with an empty error string";
		return v;
	}

	Verdict ErrorAsSuccess(const std::string& why)
	{
		Verdict v;
		v.code = kErrorAsSuccess;
		v.detail = why;
		return v;
	}

	// ---------------------------------------------------------------------------
	// a memory ceiling the harness can see
	//
	// The parsers do not call malloc themselves (checked: no malloc/calloc/
	// realloc in pfs, ipt, iet, asb, luaast, lua, ast), so every byte they ask
	// for arrives through operator new.  Counting live bytes here is therefore a
	// precise per-parse measurement rather than a guess, and it can fail the
	// input instead of the machine: a declared 40000x40000 canvas must be
	// *refused by the parser*, so hitting this ceiling is a finding, not a pass.
	// ---------------------------------------------------------------------------
	size_t g_liveBytes = 0;
	size_t g_peakBytes = 0;
	size_t g_totalBytes = 0;
	size_t g_ceiling = 512ull * 1024ull * 1024ull;
	bool g_ceilingArmed = false;

	void NoteAlloc(size_t n, size_t live)
	{
		if(!g_ceilingArmed)
			return;
		g_totalBytes += n;
		if(live > g_peakBytes)
			g_peakBytes = live;
		if(live <= g_ceiling)
			return;
		// write through the raw call so this cannot allocate
		char message[160];
		const int length = snprintf(message, sizeof(message),
			"oom: live %zu bytes, ceiling %zu, asking for %zu\n", g_liveBytes, g_ceiling, n);
		ssize_t ignored = write(2, message, (size_t)length);
		(void)ignored;
		_exit(kOom);
	}
}

// Global replacements: everything a parser allocates goes through these.  A
// 32-byte header records the size so `delete` can subtract it and "peak live" is
// a real number rather than "everything ever asked for".  32 keeps the 16-byte
// alignment malloc guarantees, and the pointer handed out still points at real
// malloc'd memory, so AddressSanitizer keeps tracking it.
namespace
{
	struct AllocHeader
	{
		size_t size;
		size_t check;
	};
	const size_t kAllocCheck = 0xA110CA7EDEADBEEFull;
	const size_t kHeaderBytes = sizeof(AllocHeader) + 16;
}

void* operator new(size_t n)
{
	const size_t wanted = n == 0 ? 1 : n;
	uint8_t* raw = (uint8_t*)malloc(wanted + kHeaderBytes);
	if(raw == nullptr)
		_exit(kOom);
	AllocHeader* header = (AllocHeader*)raw;
	header->size = wanted;
	header->check = kAllocCheck;
	g_liveBytes += wanted;
	NoteAlloc(wanted, g_liveBytes);
	return raw + kHeaderBytes;
}

void* operator new[](size_t n) { return operator new(n); }

void operator delete(void* p) noexcept
{
	if(p == nullptr)
		return;
	uint8_t* raw = (uint8_t*)p - kHeaderBytes;
	AllocHeader* header = (AllocHeader*)raw;
	if(header->check == kAllocCheck)
	{
		g_liveBytes -= (header->size < g_liveBytes) ? header->size : g_liveBytes;
		header->check = 0;
	}
	free(raw);
}

void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, size_t) noexcept { operator delete(p); }
void operator delete[](void* p, size_t) noexcept { operator delete(p); }

namespace
{
	// ---------------------------------------------------------------------------
	// the parsers, each with the invariants a *successful* parse must satisfy
	// ---------------------------------------------------------------------------
	Verdict RunPfs(const std::vector<uint8_t>& input)
	{
		// pfs::Read signals failure by throwing, so the exception *type* is part of
		// the contract: std::runtime_error is a refusal, anything else escaping is a
		// parser that did not check a size before using it.  (The first version of
		// this function forgot to return Good() after its try block, which reported
		// every successful archive as a silent refusal - a reminder that a fuzzer
		// needs a self test of its own.)
		artemis::Archive archive;
		bool refused = false;
		std::string reason;
		try
		{
			archive = artemis::Read(input);
		}
		catch(const std::runtime_error& e)
		{
			refused = true;
			reason = e.what();
		}
		catch(const std::exception& e)
		{
			Verdict v;
			v.code = kBadException;
			v.detail = std::string("escaped as ") + typeid(e).name() + ": " + e.what();
			return v;
		}
		catch(...)
		{
			Verdict v;
			v.code = kBadException;
			v.detail = "escaped as a non-std exception";
			return v;
		}
		if(refused)
			return reason.empty() ? SilentReject() : Rejected(reason);

		if(archive.version < 2 || archive.version > 9)
			return ErrorAsSuccess("version " + std::to_string(archive.version));
		if(archive.key.size() != 0 && archive.key.size() != 20)
			return ErrorAsSuccess("index key is " + std::to_string(archive.key.size()) +
				" bytes, not 20");
		if(archive.entries.size() > 4000000u)
			return ErrorAsSuccess("entry count " + std::to_string(archive.entries.size()));
		const size_t length = archive.length();
		for(const artemis::Entry& entry : archive.entries)
		{
			if(entry.name.empty())
				return ErrorAsSuccess("an entry has an empty name");
			if(entry.name.find('\0') != std::string::npos)
				return ErrorAsSuccess("an entry name contains a NUL");
			if((uint64_t)entry.offset > (uint64_t)length)
				return ErrorAsSuccess("entry offset " + std::to_string(entry.offset) +
					" past the end (" + std::to_string(length) + ")");
			if((uint64_t)entry.offset + (uint64_t)entry.size > (uint64_t)length)
				return ErrorAsSuccess("entry '" + entry.name.substr(0, 40) + "' runs " +
					std::to_string((uint64_t)entry.offset + entry.size - length) +
					" bytes past the end");
		}
		// a listing that claims entries must actually let one be read
		if(!archive.entries.empty())
		{
			const std::vector<uint8_t> sample = artemis::Sample(archive, archive.entries[0], 64);
			if(sample.size() > archive.entries[0].size)
				return ErrorAsSuccess("Sample returned more bytes than the entry holds");
		}
		return Good();
	}

	Verdict RunAst(const std::vector<uint8_t>& input)
	{
		const std::string text((const char*)input.data(), input.size());
		artemis::LuaAst ast;
		std::string error;
		const bool ok = artemis::ParseLuaAst(text, ast, error);
		if(!ok)
			return error.empty() ? SilentReject() : Rejected(error);
		// a claimed success must be consistent with the bytes it came from
		if(ast.blocks.size() > text.size() + 1)
			return ErrorAsSuccess("more blocks (" + std::to_string(ast.blocks.size()) +
				") than bytes (" + std::to_string(text.size()) + ")");
		for(const artemis::AstBlock& block : ast.blocks)
		{
			if(block.name.size() > text.size())
				return ErrorAsSuccess("block name longer than the file");
			for(const artemis::AstCommand& command : block.commands)
				if(command.name.size() > text.size())
					return ErrorAsSuccess("command name longer than the file");
		}
		// the player-facing extraction runs on every real .ast, so it has to
		// survive whatever the parser accepted
		const std::vector<artemis::SpokenLine> lines = artemis::SpokenLines(ast);
		if(lines.size() > text.size() + 1)
			return ErrorAsSuccess("more spoken lines than bytes");
		return Good();
	}

	Verdict RunIet(const std::vector<uint8_t>& input)
	{
		const std::string decoded = artemis::DecodeScenarioText(input.data(), input.size());
		if(decoded.empty() && !input.empty())
			return Rejected("not decodable as Shift-JIS or UTF-8");
		artemis::IetScript script;
		std::string error;
		if(!artemis::IetScript::Parse(decoded, script, error))
			return error.empty() ? SilentReject() : Rejected(error);
		if(script.items().size() > decoded.size() + 1)
			return ErrorAsSuccess("more items than bytes");
		if((size_t)script.commandCount() > script.items().size())
			return ErrorAsSuccess("command count exceeds the item list");
		if((size_t)script.textCount() > script.items().size())
			return ErrorAsSuccess("text count exceeds the item list");
		int lines = 1;
		for(char c : decoded)
			if(c == '\n')
				lines++;
		for(const artemis::Item& item : script.items())
		{
			if(item.line < 1 || item.line > lines + 1)
				return ErrorAsSuccess("item reports line " + std::to_string(item.line) +
					" of " + std::to_string(lines));
			if(item.kind == artemis::Item::Kind::Command && item.text.size() > decoded.size())
				return ErrorAsSuccess("command name longer than the file");
		}
		return Good();
	}

	Verdict RunIpt(const std::vector<uint8_t>& input)
	{
		const std::string text((const char*)input.data(), input.size());
		artemis::Ipt ipt;
		std::string error;
		if(!artemis::ParseIpt(text, ipt, error))
			return error.empty() ? SilentReject() : Rejected(error);
		// Unknown is a *documented* answer, not a failure: ParseIpt reports a table it
		// could not classify by returning true with dialect Unknown and a note
		// saying why, and ComposeIpt refuses it with that name.  What would be a
		// failure is returning Unknown with no explanation at all, which is what
		// this checks.
		if(ipt.dialect == artemis::IptDialect::Unknown && ipt.notes.empty())
			return ErrorAsSuccess("parsed to dialect Unknown with no note saying why");
		if(ipt.frames.size() > text.size() + 1 || ipt.tiles.size() > text.size() + 1)
			return ErrorAsSuccess("more frames/tiles than bytes");
		if((ipt.dialect == artemis::IptDialect::Composite) && ipt.width > 0 && ipt.height > 0)
		{
			// the same ceiling ComposeIpt uses: 16384 per side and a 512 MiB canvas
			if(ipt.width > 16384 || ipt.height > 16384)
				return ErrorAsSuccess("composite canvas " + std::to_string(ipt.width) + "x" +
					std::to_string(ipt.height) + " passed the size check");
		}
		// parsing the same bytes twice must give the same picture: a difference
		// here means the parser read uninitialised memory
		artemis::Ipt again;
		std::string againError;
		if(!artemis::ParseIpt(text, again, againError))
			return ErrorAsSuccess("second parse of the same bytes failed");
		if(again.dialect != ipt.dialect || again.width != ipt.width ||
			again.height != ipt.height || again.frames.size() != ipt.frames.size() ||
			again.tiles.size() != ipt.tiles.size())
			return ErrorAsSuccess("two parses of the same bytes disagree");
		return Good();
	}

	Verdict RunAsb(const std::vector<uint8_t>& input)
	{
		artemis::Asb asb;
		std::string error;
		if(!artemis::ParseAsb(input.data(), input.size(), asb, error))
			return error.empty() ? SilentReject() : Rejected(error);
		if(asb.items.size() > asb.itemCount)
			return ErrorAsSuccess("produced " + std::to_string(asb.items.size()) +
				" items for a declared count of " + std::to_string(asb.itemCount));
		if(!asb.truncated && asb.items.size() != asb.itemCount)
			return ErrorAsSuccess("claims a complete read but " +
				std::to_string(asb.items.size()) + " of " + std::to_string(asb.itemCount) +
				" items came out");
		if(asb.truncated && asb.note.empty())
			return ErrorAsSuccess("truncated without saying why");
		for(const artemis::AsbItem& item : asb.items)
		{
			if(item.name.empty())
				return ErrorAsSuccess("an item has no name");
			if(item.name.size() > input.size())
				return ErrorAsSuccess("item name longer than the file");
			for(const artemis::AsbAttribute& attr : item.attributes)
				if(attr.name.size() > input.size() || attr.value.size() > input.size())
					return ErrorAsSuccess("attribute longer than the file");
		}
		return Good();
	}

	// ---------------------------------------------------------------------------
	// the child side of one input
	// ---------------------------------------------------------------------------
	bool ReadFile(const std::string& path, std::string& out);
	using Runner = Verdict (*)(const std::vector<uint8_t>&);

	Runner RunnerFor(const std::string& name)
	{
		if(name == "pfs")
			return RunPfs;
		if(name == "ast")
			return RunAst;
		if(name == "iet")
			return RunIet;
		if(name == "ipt")
			return RunIpt;
		if(name == "asb")
			return RunAsb;
		return nullptr;
	}

	struct Result
	{
		int code = kOk;
		int signal = 0;
		bool timedOut = false;
		double ms = 0.0;
		size_t peak = 0;
		std::string detail;
		bool good() const { return code == kOk || code == kRejected; }
	};

	double NowMs()
	{
		struct timeval tv;
		gettimeofday(&tv, nullptr);
		return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
	}

	// The child reports its verdict on a pipe and exits with the code, so the
	// parent can tell "refused politely" from "died" without decoding a core.
	Result RunInChild(const Runner runner, const std::vector<uint8_t>& input,
		int timeoutMs, size_t ceiling, bool verbose)
	{
		Result result;
		int pipeEnds[2] = { -1, -1 };
		if(pipe(pipeEnds) != 0)
		{
			result.code = kBadException;
			result.detail = "the harness could not create a pipe";
			return result;
		}
		const double started = NowMs();
		// the child's stderr goes to a file: when AddressSanitizer writes a stack
		// trace it does so on the way out, and a finding without that trace is not
		// actionable
		char errorPath[512];
		snprintf(errorPath, sizeof(errorPath), "/tmp/fx-fuzz-%d.err", (int)getpid());
		const int errorFd = open(errorPath, O_RDWR | O_CREAT | O_TRUNC, 0600);
		const pid_t pid = fork();
		if(pid == 0)
		{
			close(pipeEnds[0]);
			if(errorFd >= 0)
				dup2(errorFd, 2);
			// a parse that runs long is a finding; SIGALRM's default action ends it
			struct itimerval timer;
			memset(&timer, 0, sizeof(timer));
			timer.it_value.tv_sec = timeoutMs / 1000;
			timer.it_value.tv_usec = (timeoutMs % 1000) * 1000;
			setitimer(ITIMER_REAL, &timer, nullptr);

			g_liveBytes = 0;
			g_peakBytes = 0;
			g_ceiling = ceiling;
			g_ceilingArmed = true;
			Verdict verdict = runner(input);
			g_ceilingArmed = false;

			char header[128];
			const int length = snprintf(header, sizeof(header), "%zu %zu\n", g_peakBytes,
				verdict.detail.size());
			ssize_t ignored = write(pipeEnds[1], header, (size_t)length);
			if(!verdict.detail.empty())
				ignored = write(pipeEnds[1], verdict.detail.data(), verdict.detail.size());
			(void)ignored;
			close(pipeEnds[1]);
			_exit(verdict.code);
		}
		close(pipeEnds[1]);
		if(errorFd >= 0)
			close(errorFd);
		// read whatever the child said, with a parent-side hard deadline in case it
		// ignores SIGALRM (a spin inside a signal-blocked region)
		std::string reported;
		char buffer[512];
		const double deadline = started + (double)timeoutMs * 1.5 + 500.0;
		for(;;)
		{
			const ssize_t got = read(pipeEnds[0], buffer, sizeof(buffer));
			if(got > 0)
			{
				reported.append(buffer, (size_t)got);
				continue;
			}
			if(got == 0)
				break;
			if(errno == EINTR)
				continue;
			break;
		}
		int status = 0;
		for(;;)
		{
			const pid_t waited = waitpid(pid, &status, WNOHANG);
			if(waited == pid)
				break;
			if(NowMs() > deadline)
			{
				kill(pid, SIGKILL);
				waitpid(pid, &status, 0);
				result.timedOut = true;
				break;
			}
			usleep(2000);
		}
		close(pipeEnds[0]);
		result.ms = NowMs() - started;

		// keep the sanitizer/assert output for a bad verdict only
		std::string childError;
		if(!result.timedOut && (WIFSIGNALED(status) || WEXITSTATUS(status) != kOk))
		{
			std::string captured;
			if(ReadFile(errorPath, captured))
			{
				// the interesting part is the top of a sanitizer report or the tail
				// of an assertion, so keep the first 1600 bytes
				if(captured.size() > 1600)
					captured.resize(1600);
				childError = captured;
			}
		}
		unlink(errorPath);

		if(result.timedOut)
		{
			result.code = -1;
			result.detail = "exceeded the " + std::to_string(timeoutMs) + " ms deadline";
			return result;
		}
		if(WIFSIGNALED(status))
		{
			result.signal = WTERMSIG(status);
			result.code = -2;
			result.detail = std::string("killed by ") + strsignal(result.signal);
			if(!childError.empty())
				result.detail += "\n" + childError;
			return result;
		}
		if(!WIFEXITED(status))
		{
			result.code = kBadException;
			result.detail = "child neither exited nor signalled";
			return result;
		}
		result.code = WEXITSTATUS(status);
		if(result.code == kAsanExit)
		{
			// ASAN_OPTIONS=exitcode=42: the sanitizer found something and stopped.
			// Without this mapping it would arrive here as "rejected" (exit 1 is
			// how a parser refuses) and the crash would be reported as a pass.
			result.code = -3;
			result.detail = "AddressSanitizer stopped the parse";
			if(!childError.empty())
				result.detail += "\n" + childError;
			return result;
		}
		if(result.code != kOk && result.code != kRejected && !childError.empty())
			result.detail += "\n" + childError;
		// "<peak> <detail-length>\n<detail>"
		const size_t space = reported.find(' ');
		const size_t newline = reported.find('\n');
		if(space != std::string::npos && newline != std::string::npos && space < newline)
		{
			result.peak = (size_t)strtoull(reported.substr(0, space).c_str(), nullptr, 10);
			const size_t declared = (size_t)strtoull(reported.c_str() + space + 1, nullptr, 10);
			if(newline + 1 <= reported.size())
				result.detail = reported.substr(newline + 1, declared);
		}
		if(result.code == kOom && result.detail.empty())
			result.detail = "live allocations passed the ceiling";
		if(verbose)
		{
			fprintf(stderr, "  verdict %s after %.1f ms, peak %zu bytes: %s\n",
				VerdictName(result.code), result.ms, result.peak, result.detail.c_str());
		}
		return result;
	}

	// ---------------------------------------------------------------------------
	// seeds
	// ---------------------------------------------------------------------------
	bool ReadFile(const std::string& path, std::string& out)
	{
		FILE* file = fopen(path.c_str(), "rb");
		if(file == nullptr)
			return false;
		char buffer[65536];
		size_t got = 0;
		while((got = fread(buffer, 1, sizeof(buffer), file)) > 0)
			out.append(buffer, got);
		fclose(file);
		return true;
	}


	struct Seed
	{
		std::string name;
		std::vector<uint8_t> bytes;
	};

	void AddFileSeed(std::vector<Seed>& seeds, const std::string& path, size_t limit)
	{
		std::string bytes;
		if(!ReadFile(path, bytes))
			return;
		if(bytes.size() > limit)
			bytes.resize(limit);
		Seed seed;
		seed.name = path;
		seed.bytes.assign(bytes.begin(), bytes.end());
		seeds.push_back(seed);
	}

	void AddSeed(std::vector<Seed>& seeds, const std::string& name, const std::string& text)
	{
		Seed seed;
		seed.name = name;
		seed.bytes.assign(text.begin(), text.end());
		seeds.push_back(seed);
	}

	std::vector<Seed> LoadSeeds(const std::string& parser, const std::string& corpus)
	{
		std::vector<Seed> seeds;
		const std::string nicolan = corpus + "/artemis-nicolan/extracted";
		const std::string samples = corpus + "/artemis-raspberry-web/samples";
		const std::string henpri = corpus + "/artemis-henpri-trial/extracted";

		if(parser == "pfs")
		{
			AddFileSeed(seeds, nicolan + "/../root.pfs", 512 * 1024);
			AddFileSeed(seeds, corpus + "/artemis-henpri-trial/raw/HENPRI.pfs.060", 256 * 1024);
			// the corpus only ships pf6/pf8; the other generations get seeds from the
			// writer so every branch of the index reader is reachable
			const std::vector<std::pair<std::string, std::vector<uint8_t>>> files = {
				{ "root\\image\\a.png", std::vector<uint8_t>(64, 0x89) },
				{ "root\\text\\b.txt", std::vector<uint8_t>{ 'h', 'i' } },
			};
			for(const char* magic : { "pf2", "pf4", "pf5", "pf6", "pf8", "pf9" })
			{
				const std::vector<uint8_t> built = artemis::Build(files, magic, true);
				seeds.push_back({ std::string("built-") + magic,
					std::vector<uint8_t>(built.begin(), built.end()) });
			}
			const std::vector<uint8_t> v2 = artemis::BuildVersion2(files);
			seeds.push_back({ "built-pf2-v2", std::vector<uint8_t>(v2.begin(), v2.end()) });
		}
		else if(parser == "ast")
		{
			AddFileSeed(seeds, henpri + "/script/d01_c08_6.ast", 192 * 1024);
			AddFileSeed(seeds, henpri + "/script/d09_hy03.ast", 192 * 1024);
			AddFileSeed(seeds, samples + "/script_オープニング続き.ast", 192 * 1024);
			AddSeed(seeds, "synthetic-chained",
				"astver = 2.0\nast = {\n\tblock_00000 = {\n\t\t{\"text\"},\n"
				"\t\ttext = { ja = { { name = {\"少女\"}, \"「そうだな」\", {\"rt2\"} } } },\n"
				"\t\tlinknext = \"block_00001\",\n\t\tline = 20,\n\t},\n}\n");
			AddSeed(seeds, "synthetic-arrayed",
				"ast = {\n\t{\n\t\t{\"savetitle\", text=\"忠犬大将\"},\n\t\t{\"bgm\", file=\"bgm001\"},\n"
				"\t\t{\"text\"},\n\t\tcrc = \"e2d16723\",\n\t\tlang = \"lang_00000\",\n\t},\n}\n");
		}
		else if(parser == "iet")
		{
			AddFileSeed(seeds, nicolan + "/scenario/scene.iet", 256 * 1024);
			AddFileSeed(seeds, nicolan + "/first.iet", 128 * 1024);
			AddFileSeed(seeds, nicolan + "/macro.iet", 128 * 1024);
			AddFileSeed(seeds, samples + "/system_first.iet", 96 * 1024);
			AddSeed(seeds, "synthetic-kag",
				"*start\r\n[adv]\r\n[bg_b]\r\n[n]\r\n@[rp]\r\n"
				"[if estimate=\"$t.system == 0\"]\r\n[sfade time=\"2000\" gain=\"500\"]\r\n[/if]\r\n"
				"[ruby text=\"おりひめ\"]織姫[/ruby]と[ruby text=\"ひこぼし\"]彦星[/ruby]@[rp]\r\n");
			AddSeed(seeds, "synthetic-lua", "[lua]\nlocal t = {1,2,3}\n[/lua]\n[rp]\n");
		}
		else if(parser == "ipt")
		{
			AddFileSeed(seeds, samples + "/ipt/image_hd_anime_line1.ipt", 64 * 1024);
			AddFileSeed(seeds, samples + "/ipt/psw_ui_ja_mw_dialog.ipt", 64 * 1024);
			AddFileSeed(seeds, samples + "/ipt/psw_ui_ja_mw_map.ipt", 64 * 1024);
			AddFileSeed(seeds, samples + "/ipt/psw_ui_ja_save_char.ipt", 64 * 1024);
			AddSeed(seeds, "synthetic-cut",
				"ipt = { mode = \"cut\", base = { w = 1280, h = 720, x = 0, y = 0 },\n"
				"\t{ id = 0, file = \"ev01_00\", x = 0, y = 0 },\n}\n");
			AddSeed(seeds, "synthetic-diff",
				"ipt = { mode = \"diff\", base = { \"ev01_base\", w = 1280, h = 720, x = 0, y = 0 },\n"
				"\t{ id = 0, file = \"ev01_00\", x = 0, y = 0 },\n}\n");
			AddSeed(seeds, "synthetic-atlas",
				"ipt = { ok_qs = \"576,0,424,36\", [\"瑠莉\"] = \"0,1,166,93\" }\n");
			AddSeed(seeds, "synthetic-data",
				"ipt = { { x=50, y=394 }, trial = { ruri = { 1, 1, 5 } } }\n");
		}
		else if(parser == "asb")
		{
			AddFileSeed(seeds, samples + "/asb/system_script.asb", 64 * 1024);
			AddFileSeed(seeds, samples + "/asb/system_save.asb", 64 * 1024);
		}
		return seeds;
	}

	// ---------------------------------------------------------------------------
	// structural mutation
	//
	// The list is the interesting part: each kind stands for a way real damaged
	// data arrives.  A uniform random byte flip finds different bugs than "this
	// length field times ten" or "the second byte of a Shift-JIS character lost",
	// and the corpus has already shown that the latter two are what shipped
	// scripts actually contain.
	// ---------------------------------------------------------------------------
	enum MutationKind
	{
		kFlipBit,
		kFlipBytes,
		kTruncate,
		kTruncateTiny,
		kInsertNoise,
		kDeleteRange,
		kDuplicateRange,
		kSwapRanges,
		kZeroRange,
		kFillFF,
		kLengthTimesTen,
		kLengthMax,
		kAppendJunk,
		kNegativeNumber,
		kBreakUtf8,
		kHalfSjis,
		kOpenBrackets,
		kRepeatLine,
		kDeepNest,
		kLongLine,
		kPureRandom,
		kMutationKinds
	};

	const char* MutationName(int kind)
	{
		static const char* names[kMutationKinds] = {
			"flip-bit", "flip-bytes", "truncate", "truncate-tiny", "insert-noise",
			"delete-range", "duplicate-range", "swap-ranges", "zero-range", "fill-ff",
			"length-times-ten", "length-max", "append-junk", "negative-number",
			"break-utf8", "half-sjis", "open-brackets", "repeat-line", "deep-nest",
			"long-line", "pure-random",
		};
		return (kind >= 0 && kind < kMutationKinds) ? names[kind] : "?";
	}

	size_t RandomIndex(std::mt19937_64& rng, size_t bound)
	{
		if(bound == 0)
			return 0;
		return (size_t)(rng() % (uint64_t)bound);
	}

	uint8_t RandomByte(std::mt19937_64& rng) { return (uint8_t)(rng() & 0xFF); }

	// A u32 little-endian at `at`, with the shape a length or offset field has.
	bool ReadU32(const std::vector<uint8_t>& data, size_t at, uint32_t& out)
	{
		if(at + 4 > data.size())
			return false;
		out = (uint32_t)data[at] | ((uint32_t)data[at + 1] << 8) |
			((uint32_t)data[at + 2] << 16) | ((uint32_t)data[at + 3] << 24);
		return true;
	}

	void WriteU32(std::vector<uint8_t>& data, size_t at, uint32_t value)
	{
		if(at + 4 > data.size())
			return;
		data[at] = (uint8_t)(value & 0xFF);
		data[at + 1] = (uint8_t)((value >> 8) & 0xFF);
		data[at + 2] = (uint8_t)((value >> 16) & 0xFF);
		data[at + 3] = (uint8_t)((value >> 24) & 0xFF);
	}

	// Where the length fields plausibly are: a u32 whose value fits inside the
	// buffer (a length) or inside 4x the buffer (an offset).  Mutating *those*
	// reaches the checks that a random byte flip almost never reaches.
	std::vector<size_t> LengthFieldCandidates(const std::vector<uint8_t>& data)
	{
		std::vector<size_t> out;
		const size_t limit = data.size() < 4096 ? data.size() : 4096;
		for(size_t at = 0; at + 4 <= limit; at++)
		{
			uint32_t value = 0;
			if(!ReadU32(data, at, value))
				continue;
			if(value > 0 && value <= data.size() * 4)
				out.push_back(at);
		}
		return out;
	}

	std::vector<uint8_t> Mutate(const std::vector<uint8_t>& seed, std::mt19937_64& rng,
		int kind, int* extraMutations, const std::string& parser)
	{
		std::vector<uint8_t> out = seed;
		// a small stack of extra flips makes every input a little different even
		// when the main mutation lands in a section the parser ignores
		const int extras = (int)(rng() % 4);
		if(extraMutations != nullptr)
			*extraMutations = extras;

		switch(kind)
		{
		case kFlipBit:
		{
			if(out.empty())
				break;
			const size_t at = RandomIndex(rng, out.size());
			out[at] ^= (uint8_t)(1u << (rng() % 8));
			break;
		}
		case kFlipBytes:
		{
			const size_t count = 1 + RandomIndex(rng, 16);
			for(size_t i = 0; i < count && !out.empty(); i++)
				out[RandomIndex(rng, out.size())] = RandomByte(rng);
			break;
		}
		case kTruncate:
		{
			if(out.empty())
				break;
			out.resize(RandomIndex(rng, out.size()));
			break;
		}
		case kTruncateTiny:
		{
			if(out.empty())
				break;
			out.resize(1 + RandomIndex(rng, out.size() / 8 + 1));
			break;
		}
		case kInsertNoise:
		{
			const size_t at = RandomIndex(rng, out.size() + 1);
			const size_t count = 1 + RandomIndex(rng, 64);
			std::vector<uint8_t> noise(count);
			for(size_t i = 0; i < count; i++)
				noise[i] = RandomByte(rng);
			out.insert(out.begin() + (long)at, noise.begin(), noise.end());
			break;
		}
		case kDeleteRange:
		{
			if(out.empty())
				break;
			const size_t at = RandomIndex(rng, out.size());
			const size_t count = 1 + RandomIndex(rng, out.size() - at);
			out.erase(out.begin() + (long)at, out.begin() + (long)(at + count));
			break;
		}
		case kDuplicateRange:
		{
			if(out.empty())
				break;
			const size_t at = RandomIndex(rng, out.size());
			const size_t count = 1 + RandomIndex(rng, out.size() - at);
			const std::vector<uint8_t> copy(out.begin() + (long)at,
				out.begin() + (long)(at + count));
			out.insert(out.begin() + (long)at, copy.begin(), copy.end());
			break;
		}
		case kSwapRanges:
		{
			if(out.size() < 4)
				break;
			const size_t a = RandomIndex(rng, out.size());
			const size_t b = RandomIndex(rng, out.size());
			const size_t count = 1 + RandomIndex(rng, 16);
			for(size_t i = 0; i < count && a + i < out.size() && b + i < out.size(); i++)
				std::swap(out[a + i], out[b + i]);
			break;
		}
		case kZeroRange:
		{
			if(out.empty())
				break;
			const size_t at = RandomIndex(rng, out.size());
			const size_t count = 1 + RandomIndex(rng, 32);
			for(size_t i = 0; i < count && at + i < out.size(); i++)
				out[at + i] = 0;
			break;
		}
		case kFillFF:
		{
			if(out.empty())
				break;
			const size_t at = RandomIndex(rng, out.size());
			const size_t count = 1 + RandomIndex(rng, 32);
			for(size_t i = 0; i < count && at + i < out.size(); i++)
				out[at + i] = 0xFF;
			break;
		}
		case kLengthTimesTen:
		case kLengthMax:
		{
			const std::vector<size_t> candidates = LengthFieldCandidates(out);
			if(candidates.empty())
				break;
			const size_t at = candidates[RandomIndex(rng, candidates.size())];
			uint32_t value = 0;
			if(!ReadU32(out, at, value))
				break;
			if(kind == kLengthTimesTen)
				WriteU32(out, at, value > 0xFFFFFFFFu / 10u ? 0xFFFFFFFFu : value * 10u);
			else
				WriteU32(out, at, 0xFFFFFFFFu);
			break;
		}
		case kAppendJunk:
		{
			const size_t count = 1 + RandomIndex(rng, 4096);
			for(size_t i = 0; i < count; i++)
				out.push_back(RandomByte(rng));
			break;
		}
		case kNegativeNumber:
		{
			// every decimal run in a text format, made negative: the size check that
			// forgets a '-' is a classic
			std::string text((const char*)out.data(), out.size());
			std::vector<size_t> starts;
			for(size_t i = 0; i + 1 < text.size(); i++)
				if(text[i] >= '0' && text[i] <= '9' &&
					(i == 0 || !(text[i - 1] >= '0' && text[i - 1] <= '9')) &&
					(i == 0 || text[i - 1] != '-'))
					starts.push_back(i);
			if(!starts.empty())
			{
				const size_t at = starts[RandomIndex(rng, starts.size())];
				text.insert(at, "-");
				out.assign(text.begin(), text.end());
			}
			break;
		}
		case kBreakUtf8:
		{
			// delete one byte from the middle of a multi-byte sequence: the input the
			// typewriter work already showed CP932 produces when a download ends mid
			// character
			if(out.size() < 3)
				break;
			for(int tries = 0; tries < 32; tries++)
			{
				const size_t at = RandomIndex(rng, out.size());
				const uint8_t b = out[at];
				if((b & 0xC0) == 0x80 && at > 0)
				{
					out.erase(out.begin() + (long)at);
					break;
				}
			}
			break;
		}
		case kHalfSjis:
		{
			// a CP932 lead byte whose trail was lost: the decoder must not read past
			// it, and a byte scan must not see 0x40 '@' where a character was
			if(out.size() < 2)
				break;
			for(int tries = 0; tries < 32; tries++)
			{
				const size_t at = RandomIndex(rng, out.size());
				const uint8_t b = out[at];
				if((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC))
				{
					if(at + 1 < out.size())
						out.erase(out.begin() + (long)(at + 1));
					break;
				}
			}
			break;
		}
		case kOpenBrackets:
		{
			// unclosed nesting: '{' or '[' in the format's own syntax
			const char* pair = (rng() % 2) == 0 ? "{" : "[";
			const size_t count = 1 + RandomIndex(rng, 64);
			std::string text((const char*)out.data(), out.size());
			text += "\n";
			for(size_t i = 0; i < count; i++)
			{
				text += pair;
				if(pair[0] == '{')
					text += "a = {";
			}
			out.assign(text.begin(), text.end());
			break;
		}
		case kRepeatLine:
		{
			// a duplicated block: the case that turns a "24 items" file into 24000
			std::string text((const char*)out.data(), out.size());
			const size_t lineStart = RandomIndex(rng, text.size() + 1);
			size_t lineEnd = text.find('\n', lineStart);
			if(lineEnd == std::string::npos)
				lineEnd = text.size();
			const std::string line = text.substr(lineStart, lineEnd - lineStart);
			const size_t copies = 1 + RandomIndex(rng, 64);
			for(size_t i = 0; i < copies; i++)
				text.insert(lineStart, line);
			out.assign(text.begin(), text.end());
			break;
		}
		case kDeepNest:
		{
			// the shape the resource probes use, in the campaign: thousands of levels
			// of nesting, which is what turns a recursive reader into a stack overflow
			// the measured crash thresholds: ~4000 levels under the sanitizers (a
			// 16 KiB file) and ~32000 in a release build (a 128 KiB file)
			const size_t levels = 2000 + RandomIndex(rng, 30000);
			// a document that *starts* where the reader starts, then nests: a bare
			// `{a={a=` at the top is a syntax error on the first token and reaches no
			// recursion at all (the first version of this mutator proved that)
			const std::string key = parser == "ipt" ? "ipt = " : (parser == "ast" ? "ast = " : "x = ");
			std::string text = key;
			// one kind of nesting only: mixing in `[{` made half the documents a
			// syntax error at the first bad level, so nothing ever got deep
			for(size_t i = 0; i < levels; i++)
				text += "{a=";
			text += "1";
			for(size_t i = 0; i < levels; i++)
				text += "}";
			text += "\n";
			out.assign(text.begin(), text.end());
			break;
		}
		case kLongLine:
		{
			// one line with no line break in it at all: the case with no length limit
			const size_t count = 64 * 1024 + RandomIndex(rng, 4 * 1024 * 1024);
			for(size_t i = 0; i < count; i++)
				out.push_back((uint8_t)('a' + (int)(rng() % 26)));
			break;
		}
		case kPureRandom:
		{
			const size_t count = 1 + RandomIndex(rng, 2048);
			out.assign(count, 0);
			for(size_t i = 0; i < count; i++)
				out[i] = RandomByte(rng);
			break;
		}
		default:
			break;
		}

		for(int i = 0; i < extras && !out.empty(); i++)
			out[RandomIndex(rng, out.size())] ^= (uint8_t)(1u << (rng() % 8));
		if(out.size() > 1024 * 1024)
			out.resize(1024 * 1024);
		return out;
	}

	// ---------------------------------------------------------------------------
	// minimisation: the finding has to be actionable
	// ---------------------------------------------------------------------------
	bool SameFailure(const Runner runner, const std::vector<uint8_t>& input, int wantCode,
		int timeoutMs, size_t ceiling)
	{
		const Result result = RunInChild(runner, input, timeoutMs, ceiling, false);
		if(wantCode < 0)
			return result.code == wantCode || result.timedOut || result.signal != 0;
		return result.code == wantCode;
	}

	std::vector<uint8_t> Minimise(const Runner runner, std::vector<uint8_t> input, int code,
		int timeoutMs, size_t ceiling, int budget)
	{
		int trials = 0;
		// drop halves from the tail, then single chunks
		for(size_t chunk = input.size() / 2; chunk >= 1 && trials < budget; chunk /= 2)
		{
			for(size_t at = 0; at + chunk <= input.size() && trials < budget;)
			{
				std::vector<uint8_t> candidate = input;
				candidate.erase(candidate.begin() + (long)at,
					candidate.begin() + (long)(at + chunk));
				trials++;
				if(SameFailure(runner, candidate, code, timeoutMs, ceiling))
				{
					input = candidate;
					continue;
				}
				at += chunk;
			}
			if(chunk == 1)
				break;
		}
		return input;
	}

	// ---------------------------------------------------------------------------
	// saving findings
	// ---------------------------------------------------------------------------
	std::string KindName(const Result& result)
	{
		if(result.timedOut)
			return "timeout";
		if(result.signal != 0)
			return "crash";
		if(result.code == -3)
			return "sanitizer";
		switch(result.code)
		{
		case kOk: return "ok";
		case kRejected: return "rejected";
		case kSilentReject: return "silent-reject";
		case kErrorAsSuccess: return "error-success";
		case kBadException: return "bad-exception";
		case kOom: return "oom";
		default: return "unknown";
		}
	}

	void EnsureDirectory(const std::string& path)
	{
		::mkdir(path.c_str(), 0755);
	}

	void WriteBinary(const std::string& path, const std::vector<uint8_t>& bytes)
	{
		FILE* file = fopen(path.c_str(), "wb");
		if(file == nullptr)
			return;
		if(!bytes.empty())
			fwrite(bytes.data(), 1, bytes.size(), file);
		fclose(file);
	}

	std::string StripExtension(const std::string& path)
	{
		const size_t slash = path.find_last_of('/');
		const size_t start = slash == std::string::npos ? 0 : slash + 1;
		const size_t dot = path.find_last_of('.');
		if(dot == std::string::npos || dot < start)
			return path.substr(start);
		return path.substr(start, dot - start);
	}

	struct Findings
	{
		int saved = 0;
		int duplicates = 0;
		int maxPerKind = 3; // the tenth copy of one bug is noise, not a finding
		std::vector<std::string> lines; // one line per finding, for FINDINGS.md
		std::map<std::string, int> perKind;

		bool Accept(const std::string& parser, const std::string& kind, const std::string& why)
		{
			// a "kind" here is the parser plus the first two words of the reason, so
			// twelve truncated-entry names count once while twelve different failures
			// all get saved
			std::string key = parser + "|" + kind + "|";
			size_t words = 0;
			for(size_t i = 0; i < why.size() && words < 4; i++)
			{
				if(why[i] == ' ')
					words++;
				else
					key += why[i];
			}
			const int seen = perKind[key]++;
			if(seen >= maxPerKind)
			{
				duplicates++;
				return false;
			}
			return true;
		}
	};

	void SaveFinding(const std::string& outDir, const std::string& parser,
		const std::vector<uint8_t>& input, const Result& result, int index, uint64_t rngSeed,
		const std::string& seedName, int mutation, Findings& findings)
	{
		EnsureDirectory(outDir);
		const std::string kind = KindName(result);
		char name[256];
		snprintf(name, sizeof(name), "%s-%s-%04d", parser.c_str(), kind.c_str(), index);
		const std::string base = outDir + "/" + name;
		WriteBinary(base + ".bin", input);
		FILE* note = fopen((base + ".txt").c_str(), "w");
		if(note != nullptr)
		{
			fprintf(note, "parser:   %s\n", parser.c_str());
			fprintf(note, "finding:  %s\n", kind.c_str());
			fprintf(note, "detail:   %s\n", result.detail.c_str());
			fprintf(note, "signal:   %d\n", result.signal);
			fprintf(note, "input:    %zu bytes\n", input.size());
			fprintf(note, "seed:     %s\n", seedName.c_str());
			fprintf(note, "mutation: %s\n", MutationName(mutation));
			fprintf(note, "rng:      %llu (input index %d)\n", (unsigned long long)rngSeed, index);
			fprintf(note, "repro:    ./build/fuzz repro %s.bin %s\n", base.c_str(), parser.c_str());
			fclose(note);
		}
		char line[512];
		snprintf(line, sizeof(line), "| `%s` | %s | %s | %zu B | `%s` | `%s` |",
			parser.c_str(), kind.c_str(), MutationName(mutation), input.size(),
			seedName.empty() ? "-" : StripExtension(seedName).c_str(), (name + std::string(".bin")).c_str());
		findings.lines.push_back(line);
		findings.saved++;
		fprintf(stderr, "  FINDING %s: %s (%zu bytes, %s, seed %s)\n", name, result.detail.c_str(),
			input.size(), MutationName(mutation), StripExtension(seedName).c_str());
	}

	// ---------------------------------------------------------------------------
	// one parser, N inputs
	// ---------------------------------------------------------------------------
	struct Stats
	{
		int inputs = 0;
		int ok = 0;
		int rejected = 0;
		int crash = 0;
		int timeout = 0;
		int oom = 0;
		int silent = 0;
		int errorAsSuccess = 0;
		int badException = 0;
		double slowestMs = 0.0;
		size_t peakBytes = 0;
		int kindCounts[kMutationKinds] = { 0 };
		int seedCount = 0;
	};

	int FuzzParser(const std::string& parser, const std::string& corpus, int inputs,
		uint64_t rngSeed, int timeoutMs, size_t ceiling, const std::string& outDir,
		int stopAfter, Findings& findings)
	{
		const Runner runner = RunnerFor(parser);
		if(runner == nullptr)
		{
			fprintf(stderr, "fuzz: unknown parser %s\n", parser.c_str());
			return 2;
		}
		const std::vector<Seed> seeds = LoadSeeds(parser, corpus);
		if(seeds.empty())
		{
			fprintf(stderr, "fuzz: no seeds for %s under %s\n", parser.c_str(), corpus.c_str());
			return 2;
		}
		Stats stats;
		stats.seedCount = (int)seeds.size();
		std::mt19937_64 rng(rngSeed);
		printf("%s: %d seeds, %d inputs, rng %llu, deadline %d ms, ceiling %zu MiB\n",
			parser.c_str(), (int)seeds.size(), inputs, (unsigned long long)rngSeed, timeoutMs,
			ceiling >> 20);
		fflush(stdout);

		for(int i = 0; i < inputs; i++)
		{
			const Seed& seed = seeds[RandomIndex(rng, seeds.size())];
			const int kind = (int)RandomIndex(rng, kMutationKinds);
			int extras = 0;
			std::vector<uint8_t> input = Mutate(seed.bytes, rng, kind, &extras, parser);
			stats.inputs++;
			stats.kindCounts[kind]++;
			const Result result = RunInChild(runner, input, timeoutMs, ceiling, false);
			if(result.ms > stats.slowestMs)
				stats.slowestMs = result.ms;
			if(result.peak > stats.peakBytes)
			{
				stats.peakBytes = result.peak;
				// name the worst allocator, or the number is unactionable
				fprintf(stderr, "  peak %zu KiB on input %d (%zu bytes, %s, seed %s)\n",
					result.peak / 1024, i, input.size(), MutationName(kind),
					StripExtension(seed.name).c_str());
			}

			if(result.good())
			{
				if(result.code == kOk)
					stats.ok++;
				else
					stats.rejected++;
				continue;
			}
			if(result.timedOut)
				stats.timeout++;
			else if(result.signal != 0 || result.code == -3)
				stats.crash++;
			else if(result.code == kOom)
				stats.oom++;
			else if(result.code == kSilentReject)
				stats.silent++;
			else if(result.code == kErrorAsSuccess)
				stats.errorAsSuccess++;
			else if(result.code == kBadException)
				stats.badException++;

			if(!findings.Accept(parser, KindName(result), result.detail))
				continue;
			// a finding is only worth filing if it can be reproduced
			const std::vector<uint8_t> minimal =
				Minimise(runner, input, result.code, timeoutMs, ceiling, 120);
			SaveFinding(outDir, parser, minimal, result, i, rngSeed, seed.name, kind, findings);
			if(stopAfter > 0 && findings.saved >= stopAfter)
			{
				fprintf(stderr, "  stopping after %d findings\n", findings.saved);
				break;
			}
		}

		double totalMs = 0.0;
		printf("  ok %d, rejected %d, crash %d, timeout %d, oom %d, silent %d, "
			"error-success %d, bad-exception %d\n", stats.ok, stats.rejected, stats.crash,
			stats.timeout, stats.oom, stats.silent, stats.errorAsSuccess, stats.badException);
		printf("  slowest %.1f ms, peak %zu KiB\n", stats.slowestMs, stats.peakBytes / 1024);

		// the evidence line: what was actually exercised
		printf("  mutations:");
		for(int kind = 0; kind < kMutationKinds; kind++)
			printf(" %s=%d", MutationName(kind), stats.kindCounts[kind]);
		printf("\n");
		(void)totalMs;

		// a machine-readable line for run.sh's table
		FILE* summary = fopen((outDir + "/" + parser + ".summary").c_str(), "w");
		if(summary != nullptr)
		{
			fprintf(summary, "parser=%s seeds=%d inputs=%d ok=%d rejected=%d crash=%d "
				"timeout=%d oom=%d silent=%d error_success=%d bad_exception=%d "
				"slowest_ms=%.1f peak_kib=%zu\n", parser.c_str(), stats.seedCount, stats.inputs,
				stats.ok, stats.rejected, stats.crash, stats.timeout, stats.oom, stats.silent,
				stats.errorAsSuccess, stats.badException, stats.slowestMs, stats.peakBytes / 1024);
			fclose(summary);
		}
		const int failures = stats.crash + stats.timeout + stats.oom + stats.silent +
			stats.errorAsSuccess + stats.badException;
		return failures == 0 ? 0 : 1;
	}

	// ---------------------------------------------------------------------------
	// resource-limit probes
	//
	// These are not mutations: each one is a single file a hostile or broken
	// tool could ship, and the parser is expected to *refuse* it.  Allocating
	// first and dying later is the failure this catches.
	// ---------------------------------------------------------------------------
	struct Probe
	{
		std::string parser;
		std::string name;
		std::vector<uint8_t> bytes;
		bool mustReject;
	};

	std::vector<uint8_t> AsBytes(const std::string& text)
	{
		return std::vector<uint8_t>(text.begin(), text.end());
	}

	std::vector<uint8_t> BuildPfsWithIndexSize(uint32_t indexSize, uint32_t count)
	{
		std::vector<uint8_t> data;
		data.push_back('p');
		data.push_back('f');
		data.push_back('8');
		for(int i = 0; i < 4; i++)
			data.push_back((uint8_t)((indexSize >> (8 * i)) & 0xFF));
		// an entry whose offset and size are enormous
		const std::string name = "root\\a.png";
		for(char c : name)
			data.push_back((uint8_t)c);
		for(int i = 0; i < 4; i++)
			data.push_back(0);
		for(int i = 0; i < 4; i++)
			data.push_back(0xFF); // offset
		for(int i = 0; i < 4; i++)
			data.push_back(0xFF); // size
		for(int i = 0; i < 4; i++)
			data.push_back((uint8_t)((count >> (8 * i)) & 0xFF));
		return data;
	}

	// pf8 layout: magic(3) indexSize(4) count(4) entries...  The count sits at
	// 0x07 here, unlike pf2 where it sits after the entries.
	std::vector<uint8_t> BuildPf8Header(uint32_t indexSize, uint32_t count, size_t total)
	{
		std::vector<uint8_t> data = { 'p', 'f', '8' };
		for(int i = 0; i < 4; i++)
			data.push_back((uint8_t)((indexSize >> (8 * i)) & 0xFF));
		for(int i = 0; i < 4; i++)
			data.push_back((uint8_t)((count >> (8 * i)) & 0xFF));
		while(data.size() < total)
			data.push_back(0x41);
		return data;
	}

	int RunLimits(const std::string& corpus, const std::string& outDir, int timeoutMs)
	{
		(void)corpus;
		EnsureDirectory(outDir);
		std::vector<Probe> probes;

		// .ipt: a canvas that cannot be decoded
		probes.push_back({ "ipt", "canvas-100000x100000",
			AsBytes("ipt = { mode = \"cut\", base = { w = 100000, h = 100000, x = 0, y = 0 },\n"
				"\t{ id = 0, file = \"a\", x = 0, y = 0 },\n}\n"), true });
		probes.push_back({ "ipt", "canvas-negative",
			AsBytes("ipt = { mode = \"cut\", base = { w = -1280, h = -720, x = 0, y = 0 },\n"
				"\t{ id = 0, file = \"a\", x = 0, y = 0 },\n}\n"), true });
		probes.push_back({ "ipt", "canvas-zero",
			AsBytes("ipt = { mode = \"cut\", base = { w = 0, h = 0, x = 0, y = 0 },\n"
				"\t{ id = 0, file = \"a\", x = 0, y = 0 },\n}\n"), true });
		probes.push_back({ "ipt", "canvas-overflow-multiply",
			AsBytes("ipt = { mode = \"cut\", base = { w = 65536, h = 65536, x = 0, y = 0 },\n"
				"\t{ id = 0, file = \"a\", x = 0, y = 0 },\n}\n"), true });
		probes.push_back({ "ipt", "canvas-4g-value",
			AsBytes("ipt = { mode = \"cut\", base = { w = 4294967296, h = 4294967296, x = 0, y = 0 },\n"
				"\t{ id = 0, file = \"a\", x = 0, y = 0 },\n}\n"), true });

		// .ast: nesting deep enough to run the stack out
		{
			std::string text = "ast = {";
			for(int i = 0; i < 10000; i++)
				text += "{a={";
			for(int i = 0; i < 10000; i++)
				text += "}}";
			text += "}\n";
			probes.push_back({ "ast", "nesting-10000", AsBytes(text), true });
		}
		{
			// the same depth but closed: a parser may accept it, it may refuse it,
			// it must not die - mustReject false means "either answer is fine"
			std::string text = "ast = ";
			for(int i = 0; i < 5000; i++)
				text += "{a=";
			text += "1";
			for(int i = 0; i < 5000; i++)
				text += "}";
			text += "\n";
			probes.push_back({ "ast", "nesting-5000-closed", AsBytes(text), false });
		}

		// .iet: a single line 100 MB long, and 200k tags on one line
		{
			std::vector<uint8_t> bytes;
			bytes.reserve(100u * 1024u * 1024u + 16);
			for(size_t i = 0; i < 100u * 1024u * 1024u; i++)
				bytes.push_back((uint8_t)('a' + (i % 26)));
			probes.push_back({ "iet", "one-line-100mb", bytes, false });
		}
		{
			std::string text;
			text.reserve(200000 * 8);
			for(int i = 0; i < 200000; i++)
				text += "[rp]";
			text += "\r\n";
			probes.push_back({ "iet", "tags-200000-one-line", AsBytes(text), false });
		}

		// .asb: a header that lies about the item count
		{
			std::vector<uint8_t> bytes = { 'A', 'S', 'B', 0, 0 };
			const uint32_t count = 0xFFFFFFFFu;
			for(int i = 0; i < 4; i++)
				bytes.push_back((uint8_t)((count >> (8 * i)) & 0xFF));
			probes.push_back({ "asb", "item-count-max", bytes, true });
		}
		{
			std::vector<uint8_t> bytes = { 'A', 'S', 'B', 0, 0 };
			const uint32_t count = 1000000;
			for(int i = 0; i < 4; i++)
				bytes.push_back((uint8_t)((count >> (8 * i)) & 0xFF));
			const uint32_t firstType = 0;
			for(int i = 0; i < 4; i++)
				bytes.push_back((uint8_t)((firstType >> (8 * i)) & 0xFF));
			probes.push_back({ "asb", "item-count-1m-no-body", bytes, true });
		}

		// .pfs: an entry count the index cannot possibly hold.  The reader does check
		// that (`entry runs past the index`), but it reserves the full count first:
		// this probe measures what that costs for a 1 KiB file.
		probes.push_back({ "pfs", "count-3.9m-small-index",
			BuildPf8Header(16, 3900000, 1024), false });

		// .pfs: an index that claims 4 GiB
		probes.push_back({ "pfs", "index-size-4g", BuildPfsWithIndexSize(0xFFFFFFFFu, 1), true });
		probes.push_back({ "pfs", "entry-count-4g", BuildPfsWithIndexSize(64, 0xFFFFFFFFu), true });

		printf("resource-limit probes\n");
		int failures = 0;
		for(const Probe& probe : probes)
		{
			const Runner runner = RunnerFor(probe.parser);
			const Result result = RunInChild(runner, probe.bytes, timeoutMs,
				512ull * 1024ull * 1024ull, false);
			const char* verdict = "?";
			bool bad = false;
			if(result.timedOut)
			{
				verdict = "TIMEOUT";
				bad = true;
			}
			else if(result.signal != 0 || result.code == -3)
			{
				verdict = "CRASH";
				bad = true;
			}
			else if(result.code == kOk)
			{
				verdict = "ACCEPTED";
				bad = probe.mustReject;
			}
			else if(result.code == kRejected)
			{
				verdict = "refused";
				bad = false;
			}
			else if(result.code == kOom)
			{
				verdict = "OOM";
				bad = true;
			}
			else if(result.code == kSilentReject)
			{
				verdict = "refused-silently";
				bad = true;
			}
			else
			{
				verdict = VerdictName(result.code);
				bad = true;
			}
			if(bad)
				failures++;
			printf("  %-4s %-24s %-16s %7.1f ms peak %6zu KiB  %s\n", probe.parser.c_str(),
				probe.name.c_str(), verdict, result.ms, result.peak / 1024,
				result.detail.substr(0, 100).c_str());
			if(bad)
			{
				char name[256];
				snprintf(name, sizeof(name), "limits-%s-%s", probe.parser.c_str(),
					probe.name.c_str());
				WriteBinary(outDir + "/" + name + ".bin", probe.bytes);
			}
		}
		printf("  %d of %zu probes failed\n", failures, probes.size());
		return failures == 0 ? 0 : 1;
	}

	// ---------------------------------------------------------------------------
	// findings: every documented finding as a file, generated from code
	//
	// The campaign saves whatever mutation happens to hit a bug, which means the
	// names change from run to run.  These are the same failures with fixed names
	// and a note beside each, so FINDINGS.md can point at something that exists
	// and the owner of each parser can reproduce in one command without a fuzzing
	// run.  Byte-for-byte deterministic: no RNG here.
	// ---------------------------------------------------------------------------
	void WriteProbe(const std::string& outDir, const std::string& name,
		const std::string& parser, const std::string& finding, const std::string& why,
		const std::vector<uint8_t>& bytes)
	{
		EnsureDirectory(outDir);
		const std::string base = outDir + "/" + name;
		WriteBinary(base, bytes);
		FILE* note = fopen((base + ".txt").c_str(), "w");
		if(note == nullptr)
			return;
		fprintf(note, "parser:  %s\n", parser.c_str());
		fprintf(note, "finding: %s\n", finding.c_str());
		fprintf(note, "why:     %s\n", why.c_str());
		fprintf(note, "size:    %zu bytes\n", bytes.size());
		fprintf(note, "stack:   ./build/fuzz direct %s %s\n", base.c_str(), parser.c_str());
		fprintf(note, "verdict: ./build/fuzz repro %s %s\n", base.c_str(), parser.c_str());
		fclose(note);
	}

	std::vector<uint8_t> NestedLua(const std::string& prefix, int depth)
	{
		std::string text = prefix;
		for(int i = 0; i < depth; i++)
			text += "{a=";
		text += "1";
		for(int i = 0; i < depth; i++)
			text += "}";
		text += "\n";
		return AsBytes(text);
	}

	int WriteFindings(const std::string& outDir)
	{
		// F1: the Lua reader recurses with no depth limit.  Both .ast and .ipt go
		// through it, so one bug has two front doors.
		WriteProbe(outDir, "f1-lua-nesting-40000.ast", "ast", "crash (stack overflow)",
			"ParseLuaAst -> lua.cpp ParseValue/ParseObject recurse once per nesting "
			"level with no depth limit; 40000 levels (a 160 KiB file) segfault in a "
			"release build, ~4000 levels crash under the sanitizers",
			NestedLua("ast = ", 40000));
		WriteProbe(outDir, "f1-lua-nesting-40000.ipt", "ipt", "crash (stack overflow)",
			"the same reader behind ParseIpt: a .ipt is also a Lua table",
			NestedLua("ipt = ", 40000));

		// F2: a number outside int range is cast to int, which is undefined
		WriteProbe(outDir, "f2-ipt-canvas-4g.ipt", "ipt", "bad-exception (UB)",
			"ipt.cpp:46 IntValue casts a Lua number to int; 4294967296 is out of "
			"range (UBSan: \"outside the range of representable values of type int\")",
			AsBytes("ipt = { mode = \"cut\", base = { w = 4294967296, h = 4294967296, x = 0, y = 0 },\n"
				"\t{ id = 0, file = \"a\", x = 0, y = 0 },\n}\n"));

		// F3: no line-length limit in the .iet reader
		{
			std::vector<uint8_t> bytes;
			const size_t length = 1024u * 1024u;
			bytes.reserve(length + 8);
			for(size_t i = 0; i < length; i++)
				bytes.push_back((uint8_t)('a' + (i % 26)));
			bytes.push_back('\r');
			bytes.push_back('\n');
			WriteProbe(outDir, "f3-iet-one-line-1mb.iet", "iet", "resource (no limit)",
				"one 1 MiB line is accepted; the 100 MiB probe (run.sh limits) takes "
				"2.7 s and 492 MiB, so there is no line-length ceiling", bytes);
		}

		// F4: entry names are taken verbatim.  A NUL truncates the path that
		// pfstool builds, and `..` walks out of the extraction directory.
		{
			const std::string nulName("root\\a\0b.png", 13);
			const std::vector<std::pair<std::string, std::vector<uint8_t>>> files = {
				{ nulName, std::vector<uint8_t>{ 'x' } },
			};
			const std::vector<uint8_t> bytes = artemis::Build(files, "pf8", false);
			WriteProbe(outDir, "f4a-pfs-entry-name-nul.pfs", "pfs", "error-success",
				"an entry name with an embedded NUL is accepted; tools/pfstool.cpp:140 "
				"builds the output path from it, so fopen() writes a truncated name", bytes);
		}
		{
			const std::vector<std::pair<std::string, std::vector<uint8_t>>> files = {
				{ "..\\..\\escaped.txt", std::vector<uint8_t>{ 'p', 'w', 'n', 'e', 'd' } },
			};
			const std::vector<uint8_t> bytes = artemis::Build(files, "pf8", false);
			WriteProbe(outDir, "f4b-pfs-entry-name-traversal.pfs", "pfs", "note (tool)",
				"NormalizeName (src/pfs.cpp:551) lowercases and fixes slashes but does not "
				"remove '..'; pfstool extract then writes outside its target directory",
				bytes);
		}

		// F5: the entry count is reserved before it is checked against the index
		WriteProbe(outDir, "f5-pfs-count-3.9m-small-index.pfs", "pfs", "resource (amplification)",
			"src/pfs.cpp:336 reserves `count` entries before the loop; a 1 KiB file "
			"declaring 3.9M entries makes the reader reserve 238 MiB, then throw",
			BuildPf8Header(16, 3900000, 1024));

		printf("wrote the documented findings to %s\n", outDir.c_str());
		return 0;
	}

	// ---------------------------------------------------------------------------
	// repro: what a developer runs on a saved file
	// ---------------------------------------------------------------------------
	int Repro(const std::string& path, const std::string& parser, int timeoutMs)
	{
		const Runner runner = RunnerFor(parser);
		if(runner == nullptr)
		{
			fprintf(stderr, "fuzz: unknown parser %s\n", parser.c_str());
			return 2;
		}
		std::string bytes;
		if(!ReadFile(path, bytes))
		{
			fprintf(stderr, "fuzz: cannot read %s\n", path.c_str());
			return 2;
		}
		const std::vector<uint8_t> input(bytes.begin(), bytes.end());
		const Result result = RunInChild(runner, input, timeoutMs, 512ull * 1024ull * 1024ull,
			true);
		printf("%s: %s after %.1f ms, peak %zu bytes\n", path.c_str(),
		KindName(result).c_str(), result.ms, result.peak);
		printf("  %s\n", result.detail.c_str());
		return result.good() ? 0 : 1;
	}

	void Usage()
	{
		printf("usage: fuzz <pfs|ast|iet|ipt|asb|all> [--inputs N] [--rng N] [--timeout MS]\n"
			"            [--memcap MiB] [--corpus DIR] [--out DIR] [--stop-after N]\n"
			"       fuzz limits [--corpus DIR] [--out DIR]\n"
			"       fuzz repro <file> <parser>   (fork, deadline bound)\n"
			"       fuzz direct <file> <parser>  (in-process, sanitizer output on the terminal)\n"
			"       fuzz findings [--out DIR]    write every documented finding as a file\n");
	}
} // namespace

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		Usage();
		return 2;
	}
	const std::string command = argv[1];
	if(command == "-h" || command == "--help")
	{
		Usage();
		return 0;
	}

	int inputs = 5000;
	uint64_t rngSeed = 1;
	int timeoutMs = 2000;
	size_t ceiling = 512ull * 1024ull * 1024ull;
	std::string corpus = "../../testdata";
	std::string outDir = "crashes";
	int stopAfter = 0;

	int positionalStart = 2;
	if(command == "limits")
	{
		for(int i = 2; i < argc; i++)
		{
			const std::string arg = argv[i];
			if(arg == "--corpus" && i + 1 < argc)
				corpus = argv[++i];
			else if(arg == "--out" && i + 1 < argc)
				outDir = argv[++i];
			else if(arg == "--timeout" && i + 1 < argc)
				timeoutMs = atoi(argv[++i]);
		}
		return RunLimits(corpus, outDir, timeoutMs);
	}
	if(command == "findings")
	{
		std::string dir = "crashes";
		for(int i = 2; i < argc; i++)
		{
			const std::string arg = argv[i];
			if(arg == "--out" && i + 1 < argc)
				dir = argv[++i];
		}
		return WriteFindings(dir);
	}
	if(command == "direct")
	{
		// run the parse in this process: a sanitizer report then goes straight to
		// the terminal and to lldb, which is what a developer needs
		if(argc < 4)
		{
			Usage();
			return 2;
		}
		const Runner runner = RunnerFor(argv[3]);
		if(runner == nullptr)
		{
			fprintf(stderr, "fuzz: unknown parser %s\n", argv[3]);
			return 2;
		}
		std::string bytes;
		if(!ReadFile(argv[2], bytes))
		{
			fprintf(stderr, "fuzz: cannot read %s\n", argv[2]);
			return 2;
		}
		g_ceiling = 512ull * 1024ull * 1024ull;
		g_ceilingArmed = true;
		const std::vector<uint8_t> input(bytes.begin(), bytes.end());
		const Verdict verdict = runner(input);
		printf("%s: %s  %s\n", argv[2], VerdictName(verdict.code), verdict.detail.c_str());
		printf("peak %zu KiB over %zu bytes of input\n", g_peakBytes / 1024, input.size());
		return verdict.code == kOk || verdict.code == kRejected ? 0 : 1;
	}
	if(command == "repro")
	{
		if(argc < 4)
		{
			Usage();
			return 2;
		}
		return Repro(argv[2], argv[3], 8000);
	}

	std::vector<std::string> parsers;
	if(command == "all")
	{
		parsers = { "pfs", "ast", "iet", "ipt", "asb" };
	}
	else
	{
		if(RunnerFor(command) == nullptr)
		{
			fprintf(stderr, "fuzz: unknown parser %s\n", command.c_str());
			Usage();
			return 2;
		}
		parsers.push_back(command);
		positionalStart = 2;
	}
	for(int i = positionalStart; i < argc; i++)
	{
		const std::string arg = argv[i];
		if(arg == "--inputs" && i + 1 < argc)
			inputs = atoi(argv[++i]);
		else if(arg == "--rng" && i + 1 < argc)
			rngSeed = strtoull(argv[++i], nullptr, 10);
		else if(arg == "--timeout" && i + 1 < argc)
			timeoutMs = atoi(argv[++i]);
		else if(arg == "--memcap" && i + 1 < argc)
			ceiling = (size_t)atoll(argv[++i]) * 1024ull * 1024ull;
		else if(arg == "--corpus" && i + 1 < argc)
			corpus = argv[++i];
		else if(arg == "--out" && i + 1 < argc)
			outDir = argv[++i];
		else if(arg == "--stop-after" && i + 1 < argc)
			stopAfter = atoi(argv[++i]);
		else
		{
			fprintf(stderr, "fuzz: unknown option %s\n", arg.c_str());
			return 2;
		}
	}

	EnsureDirectory(outDir);
	Findings findings;
	int bad = 0;
	for(const std::string& parser : parsers)
	{
		const int code = FuzzParser(parser, corpus, inputs, rngSeed, timeoutMs, ceiling, outDir,
			stopAfter, findings);
		if(code == 2)
			return 2;
		if(code != 0)
			bad++;
	}

	// the table a report can paste
	FILE* table = fopen((outDir + "/findings-table.md").c_str(), "w");
	if(table != nullptr)
	{
		fprintf(table, "| parser | finding | mutation | size | seed | repro |\n");
		fprintf(table, "|---|---|---|---|---|---|\n");
		for(const std::string& line : findings.lines)
			fprintf(table, "%s\n", line.c_str());
		fclose(table);
	}
	printf("\n%d parser(s) with findings, %d finding(s) saved under %s "
		"(%d duplicate(s) of an already-saved kind not saved)\n", bad, findings.saved,
		outDir.c_str(), findings.duplicates);
	return bad == 0 ? 0 : 1;
}
