#!/bin/bash
# run.sh - the whole fuzz campaign in one command
#
#   ./run.sh                5000 inputs per parser (the requirement), then the
#                           resource-limit probes; writes crashes/ and a table
#   ./run.sh 20000          20000 inputs per parser
#   ./run.sh 5000 --no-build  reuse the binary that is already here
#
# It compiles the parsers *from source* every time instead of linking
# build/libartemis.a, for a concrete reason: macOS ships GNU make 3.81, which
# compares mtimes in whole seconds, so a source edited and rebuilt inside the
# same second silently keeps its old object file.  A fuzz campaign that tests a
# stale library is worse than no campaign, because it reports "clean".  Compiling
# directly costs about ten seconds and cannot lie.

set -u

cd "$(dirname "$0")" || exit 1
HERE="$(pwd)"
ARTEMIS="$(cd ../.. && pwd)"
INPUTS="${1:-5000}"
BUILD_REBUILD=1
for arg in "$@"; do
	case "$arg" in
	--no-build) BUILD_REBUILD=0 ;;
	esac
done

mkdir -p build crashes out

SOURCES="$ARTEMIS/src/pfs.cpp $ARTEMIS/src/sniff.cpp $ARTEMIS/src/ipt.cpp \
$ARTEMIS/src/lua.cpp $ARTEMIS/src/luaast.cpp $ARTEMIS/src/asb.cpp \
$ARTEMIS/src/iet.cpp $ARTEMIS/src/pngio.cpp"

if [ "$BUILD_REBUILD" = "1" ]; then
	PNG_CFLAGS="$(pkg-config --cflags libpng 2>/dev/null)"
	PNG_LIBS="$(pkg-config --libs libpng 2>/dev/null)"
	echo "== building (address+undefined sanitizers, sources fresh from $ARTEMIS/src)"
	# shellcheck disable=SC2086
	clang++ -std=c++17 -O1 -g -fno-omit-frame-pointer \
		-fsanitize=address,undefined -fno-sanitize-recover=all \
		-Wall -Wextra -Wno-unused-parameter \
		-I "$ARTEMIS/src" $PNG_CFLAGS \
		-o build/fuzz fuzz.cpp $SOURCES $PNG_LIBS \
		-framework CoreFoundation || exit 1
fi

# ASan must not be able to masquerade as a parser saying "no": a plain refusal
# exits 1, so ASan is told to exit 42 (the harness maps that to a finding).
export ASAN_OPTIONS="detect_leaks=0:exitcode=42:abort_on_error=0:handle_abort=1"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"

echo "== fuzzing: $INPUTS inputs per parser, 2000 ms deadline, 512 MiB ceiling"
./build/fuzz all --inputs "$INPUTS" --rng 1 --timeout 2000 --memcap 512 \
	--corpus "$ARTEMIS/../testdata" --out crashes
FUZZ_STATUS=$?

echo
echo "== resource-limit probes"
./build/fuzz limits --corpus "$ARTEMIS/../testdata" --out crashes
LIMIT_STATUS=$?

echo
echo "== summary"
for summary in crashes/*.summary; do
	[ -f "$summary" ] || continue
	# shellcheck disable=SC2002
	cat "$summary"
done

echo
TOTAL=$(cat crashes/*.summary 2>/dev/null | tr ' ' '\n' | awk -F= '/^inputs=/{s+=$2} END{print s+0}')
echo "total inputs: $TOTAL"
echo "findings saved: $(ls crashes/*.bin 2>/dev/null | wc -l | tr -d ' ')"
if [ -f crashes/findings-table.md ]; then
	echo
	cat crashes/findings-table.md
fi

if [ "$FUZZ_STATUS" = "0" ] && [ "$LIMIT_STATUS" = "0" ]; then
	echo
	echo "RESULT: clean (every input either parsed or was refused with a reason)"
	exit 0
fi
echo
echo "RESULT: findings - see FINDINGS.md and crashes/"
exit 1
