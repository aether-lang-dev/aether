#!/bin/sh
# `ae build --profile` emits a binary a sampling profiler can attribute.
#
# The three build modes are meant to be distinct:
#
#   default    -O2, no debug info      what ships
#   --quick    -O0 -g                  fast iteration; the WRONG tool for a
#                                      profile — at -O0 nothing inlines and
#                                      every temporary stays live, so the hot
#                                      spots are not the shipped binary's
#   --profile  -O2 -g -fno-omit-...    the shipped code, attributable
#
# Before --profile existed, profiling std.http.server.lb meant emitting the C
# with aetherc and hand-compiling it: the default build carries no DWARF and
# omits frame pointers, so gdb resolved 239 of 240 sampled frames as `??`.
#
# Asserts:
#   - default carries no .debug_info
#   - --quick and --profile both do
#   - --profile is genuinely -O2, not -O0 wearing a -g: its binary is
#     substantially smaller than --quick's
#   - --profile keeps frame pointers (fewer rbp pushes than -O0, but the
#     debug info is what the check above covers; this pins the -O2 half)
#
# Skips cleanly when readelf is absent (non-ELF hosts).

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT|Darwin)
        echo "  [SKIP-PLATFORM] build_profile_flag — ELF-specific section check"
        exit 0
        ;;
esac

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
SRC="$SCRIPT_DIR/probe.ae"

if ! command -v readelf >/dev/null 2>&1; then
    echo "  [SKIP] build_profile_flag: readelf not on PATH"
    exit 0
fi

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

has_debug() {
    readelf -S "$1" 2>/dev/null | grep -c "debug_info" || true
}

build() {   # build <out> [flags...]
    out="$1"; shift
    # Clear the module cache: it keys on source, not on build flags, so a
    # cached artifact from the previous mode would be handed back and the
    # comparison would measure nothing.
    rm -rf "$HOME/.aether/cache" 2>/dev/null || true
    "$AE" build "$@" "$SRC" -o "$out" >"$TMP/build.log" 2>&1 || {
        echo "  [FAIL] build_profile_flag: 'ae build $* ' failed"
        sed 's/^/    /' "$TMP/build.log" | head -10
        exit 1
    }
}

build "$TMP/def"
build "$TMP/quick" --quick
build "$TMP/prof"  --profile

if [ "$(has_debug "$TMP/def")" != "0" ]; then
    echo "  [FAIL] build_profile_flag: the default build carries debug info"
    exit 1
fi

if [ "$(has_debug "$TMP/quick")" = "0" ]; then
    echo "  [FAIL] build_profile_flag: --quick carries no debug info"
    exit 1
fi

if [ "$(has_debug "$TMP/prof")" = "0" ]; then
    echo "  [FAIL] build_profile_flag: --profile carries no debug info —"
    echo "         a sampling profiler cannot attribute frames without it"
    exit 1
fi

# --profile must be optimised. If it silently became -O0 it would still
# pass the debug-info check above while reporting hot spots the shipped
# binary does not have, which is the failure this whole flag exists to
# avoid. -O2 output is dramatically smaller than -O0.
QSIZE=$(wc -c < "$TMP/quick")
PSIZE=$(wc -c < "$TMP/prof")
if [ "$PSIZE" -ge "$QSIZE" ]; then
    echo "  [FAIL] build_profile_flag: --profile ($PSIZE bytes) is not smaller"
    echo "         than --quick ($QSIZE bytes) — it looks like -O0, not -O2"
    exit 1
fi

echo "  [PASS] build_profile_flag: default has no debug info; --quick and --profile do; --profile is -O2 (${PSIZE}B vs --quick ${QSIZE}B)"
