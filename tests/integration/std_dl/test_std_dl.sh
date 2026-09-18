#!/bin/sh
# Issue #261 prep: smoke test for std.dl cross-platform loader.
#
# Builds a tiny shared library (probe.c) into a platform-native
# .so/.dylib, then runs uses_dl.ae which dlopens it and exercises
# the std.dl API.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

# Pick the right shared-library suffix.
case "$(uname -s)" in
    Darwin) LIB_EXT=dylib ; SHARED_FLAG=-dynamiclib ;;
    Linux)  LIB_EXT=so    ; SHARED_FLAG=-shared ;;
    MINGW*|MSYS*|CYGWIN*) LIB_EXT=dll ; SHARED_FLAG=-shared ;;
    *)      LIB_EXT=so    ; SHARED_FLAG=-shared ;;
esac

LIB_PATH="$TMPDIR/libprobe.$LIB_EXT"
if ! cc $SHARED_FLAG -o "$LIB_PATH" "$SCRIPT_DIR/probe.c" 2>"$TMPDIR/cc.err"; then
    echo "  [SKIP] cannot compile shared library:"
    cat "$TMPDIR/cc.err" | head -5
    exit 0
fi

ACTUAL="$TMPDIR/actual.txt"
if ! AETHER_HOME="$ROOT" LIB_PATH="$LIB_PATH" "$AE" run "$SCRIPT_DIR/uses_dl.ae" >"$ACTUAL" 2>"$TMPDIR/err.log"; then
    echo "  [FAIL] ae run exited non-zero"
    cat "$TMPDIR/err.log" | head -10
    cat "$ACTUAL" | head -10
    exit 1
fi

EXPECTED='open: ok
symbol probe_answer: ok
symbol probe_add: ok
distinct symbols: ok
unknown symbol error: ok
close: ok'

if [ "$(cat "$ACTUAL")" != "$EXPECTED" ]; then
    echo "  [FAIL] output mismatch"
    echo "--- expected ---"
    echo "$EXPECTED"
    echo "--- actual ---"
    cat "$ACTUAL"
    exit 1
fi

echo "  [PASS] std.dl: open/symbol/close round-trip with real shared library"
