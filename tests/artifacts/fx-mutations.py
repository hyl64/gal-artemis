#!/usr/bin/env python3
# Deliberately break one fx module at a time and prove the selftest notices.
#
# Each mutation is a plausible *wrong* implementation of the thing the module
# exists to get right - the byte-sliced typewriter, the unrounded brightness,
# the absolute scetween, the stateful quake, the unbroken page, the animation
# that never ends.  A mutation that does not turn a named assertion red means
# that assertion was decoration, not a test.

import hashlib
import pathlib
import re
import shutil
import subprocess
import sys

ARTEMIS = pathlib.Path("/Users/huangyuliang/Documents/gal-engines/artemis")
BUILD = ARTEMIS / "build"

MUTATIONS = [
    (
        "typewriter: cut on bytes instead of glyphs",
        "src/fx/typewriter.cpp",
        "		out.glyphs = visible;",
        "		cut = (size_t)steps + 1; // MUTATION: naive byte cut\n		out.glyphs = visible;",
        ["typewriter-boundary"],
    ),
    (
        "brightness: drop the round-half-up (+500)",
        "src/fx/brightness.cpp",
        "((int64_t)value * (int64_t)clamped + 500) / 1000",
        "((int64_t)value * (int64_t)clamped) / 1000",
        ["brightness-500-max", "sfade-pixel-exact"],
    ),
    (
        "scetween: treat diff as an absolute value",
        "src/fx/tween.cpp",
        "		return base + ValueAt(shape, tMs);",
        "		return tween.diff; // MUTATION: absolute, not a delta",
        ["scetween-once", "scetween-twice"],
    ),
    (
        "quake: stateful sign instead of a pure hash",
        "src/fx/quake.cpp",
        "		return Mix(Mix(seed) ^ (0xD1B54A32D192ED03ull * (uint64_t)(phase + 1)));",
        "		static uint64_t bump = 0; // MUTATION: state leaks between calls\n"
        "		return Mix(Mix(seed) ^ (0xD1B54A32D192ED03ull * (uint64_t)(phase + 1)) + bump++);",
        ["quake-same-seed", "quake-hash-is-pure"],
    ),
    (
        "pages: forget that '@' is a line break",
        "src/fx/pages.cpp",
        "buffer += (c == '@') ? '\\n' : (char)c;",
        "buffer += (char)c; // MUTATION: '@' kept as text",
        ["pages-at-is-newline", "scene-page-1-lines"],
    ),
    (
        "lytween: accept a tag that has no `to`",
        "src/fx/tween.cpp",
        """		if(to == nullptr)
		{""",
        """		if(to == nullptr)
		{
			static const std::string kFrom = "0"; // MUTATION: invent a destination
			to = &kFrom;
		}
		if(false)
		{""",
        ["lytween-refuses-broken", "lytween-scene-refused"],
    ),
    (
        "animation: no loop budget, the frames never stop",
        "src/fx/animation.cpp",
        "		if(budget >= 0 && cycle >= budget)",
        "		if(false) // MUTATION: ignore loop/max",
        ["ipt-stops-after-max"],
    ),    (
        "colortone: drop the 0..255 clamp",
        "src/fx/colortone.cpp",
        """		int ClampByte(int64_t value)
		{
			if(value < 0)
				return 0;
			if(value > 255)
				return 255;
			return (int)value;
		}""",
        """		int ClampByte(int64_t value)
		{
			return (int)(uint8_t)value; // MUTATION: wrap instead of clamp
		}""",
        ["colortone-clamp-high", "colortone-clamp-low", "colortone-frame-pixels"],
    ),
    (
        "extrans: drop the half-step rounding",
        "src/fx/extrans.cpp",
        "const int64_t sum = (int64_t)a * (timeMs - tMs) + (int64_t)b * tMs + timeMs / 2;",
        "const int64_t sum = (int64_t)a * (timeMs - tMs) + (int64_t)b * tMs; // MUTATION",
        ["extrans-mid-0-255", "extrans-tmid-is-half"],
    ),
    (
        "excall: ignore an explicit return",
        "src/fx/excall.cpp",
        'else if(command.name == "return" || command.name == "excallend")',
        "else if(false) // MUTATION: no return command",
        ["excall-synthetic-return-command", "excall-return-to-caller"],
    ),
    (
        "savetitle: cut UTF-8 by bytes instead of code points",
        "src/fx/tags.cpp",
        """	std::string TruncateUtf8(const std::string& text, size_t maxBytes)
	{
		if(text.size() <= maxBytes)
			return text;""",
        """	std::string TruncateUtf8(const std::string& text, size_t maxBytes)
	{
		return text.substr(0, maxBytes); // MUTATION: byte cut
		if(text.size() <= maxBytes)
			return text;""",
        ["utf8-truncate-4", "utf8-truncate-stays-valid"],
    ),
]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build(force=None):
    # macOS ships GNU make 3.81, which compares mtimes in whole seconds: a write
    # and a build that land in the same second look "up to date" and the binary
    # silently keeps the old code.  Deleting the object file is the only reliable
    # way to force the rebuild from a script.
    if force is not None:
        # ... and the same granularity means a rebuilt .a can look no newer than
        # the executable, so the link step is skipped too.  Remove both.
        obj = BUILD / "CMakeFiles/artemis_fx.dir" / (force + ".o")
        for stale in (obj, BUILD / "libartemis_fx.a", BUILD / "fxtool"):
            if stale.exists():
                stale.unlink()
    result = subprocess.run(
        ["cmake", "--build", str(BUILD), "--target", "fxtool", "-j2"],
        capture_output=True, text=True, errors="replace")
    return result.returncode == 0, result.stdout + result.stderr


def selftest():
    result = subprocess.run([str(BUILD / "fxtool"), "selftest"],
                            capture_output=True, text=True, errors="replace", cwd=str(ARTEMIS))
    return result.stdout


def main():
    print("mutation verification\n")
    ok, log = build()
    if not ok:
        print(log[-2000:])
        sys.exit(1)
    baseline = selftest()
    if "0 failed" not in baseline:
        print("baseline is not green, refusing to mutate")
        print(baseline[-2000:])
        sys.exit(1)
    print("baseline: " + [line for line in baseline.splitlines() if "passed" in line][0] + "\n")

    verified = 0
    for title, relative, original, mutated, must_fail in MUTATIONS:
        path = ARTEMIS / relative
        before = digest(path)
        source = path.read_text()
        if source.count(original) != 1:
            print("SKIP %s (anchor not found exactly once)" % title)
            continue
        path.write_text(source.replace(original, mutated))
        try:
            built_ok, log = build(relative)
        except BaseException:
            path.write_text(source)
            raise
        if not built_ok:
            # a mutation that does not compile still proves nothing about the test
            path.write_text(source)
            print("SKIP %s (mutation did not compile)" % title)
            continue
        try:
            output = selftest()
        except BaseException:
            path.write_text(source)
            raise
        reds = set(re.findall(r"^FAIL (\S+)", output, re.MULTILINE))
        hit = [name for name in must_fail if name in reds]
        caught = len(hit) > 0
        print("%s  %s" % ("CAUGHT " if caught else "MISSED ", title))
        print("   expected red: %s" % ", ".join(must_fail))
        print("   actually red: %s" % (", ".join(sorted(reds)) if reds else "(none)"))
        if not caught:
            print("   build log tail: %s" % log[-300:].replace("\n", " | "))
        if caught:
            verified += 1
        path.write_text(source)
        built_ok, log = build(relative)
        after = digest(path)
        assert before == after, "restore failed for %s" % relative
        print("   restored %s (sha256 unchanged)\n" % relative)

    ok, log = build()
    green = selftest()
    final = [line for line in green.splitlines() if "passed" in line]
    print("after restoring every mutation: %s" % (final[0] if final else "?"))
    print("%d of %d mutations caught" % (verified, len(MUTATIONS)))
    return 0 if verified == len(MUTATIONS) and "0 failed" in green else 1


if __name__ == "__main__":
    sys.exit(main())
