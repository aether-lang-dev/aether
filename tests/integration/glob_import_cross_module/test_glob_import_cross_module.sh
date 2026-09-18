#!/bin/sh
# Issue #896 regression: glob-imported symbols across a module boundary.
#
# pathlib/module.ae does `import std.fs (*)` and calls the glob-brought
# `clean` from its exported `do_clean`. main.ae imports pathlib and calls
# pathlib.do_clean. Before the fix the merger skipped glob imports when
# rewriting a consumed module's bare references, so `clean` resolved only
# when pathlib was the entry point — consuming it failed with
# `error[E0301]: Undefined function 'clean'`.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

# Run from the test directory so `import pathlib` resolves against
# ./pathlib/module.ae.
cd "$SCRIPT_DIR"

ACTUAL="$TMPDIR/actual.txt"
if ! AETHER_HOME="$ROOT" "$AE" run main.ae >"$ACTUAL" 2>"$TMPDIR/err.log"; then
    echo "  [FAIL] ae run main.ae exited non-zero — glob import across module boundary broken"
    head -10 "$TMPDIR/err.log"
    exit 1
fi

EXPECTED="a/b"
ACTUAL_TXT=$(cat "$ACTUAL")
if [ "$ACTUAL_TXT" != "$EXPECTED" ]; then
    echo "  [FAIL] expected '$EXPECTED', got '$ACTUAL_TXT'"
    exit 1
fi

echo "  [PASS] glob_import_cross_module: pathlib's glob-brought clean() resolves when consumed"
