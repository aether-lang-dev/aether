#!/bin/sh
# Issue #243 regression: transitive module imports.
#
# main.ae imports lib_a; lib_a internally imports lib_b and calls
# lib_b.shout from inside lib_a.greet. The user must not be forced
# to also import lib_b — the merger pulls lib_b in transitively.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

# We need to run from the test directory so the local lib_a/ and lib_b/
# directories resolve as importable packages. `ae run` looks up
# `import lib_a` against ./lib_a/module.ae.
cd "$SCRIPT_DIR"

ACTUAL="$TMPDIR/actual.txt"
if ! AETHER_HOME="$ROOT" "$AE" run main.ae >"$ACTUAL" 2>"$TMPDIR/err.log"; then
    echo "  [FAIL] ae run main.ae exited non-zero — transitive merge broken"
    cat "$TMPDIR/err.log" | head -10
    exit 1
fi

EXPECTED="hello"
ACTUAL_TXT=$(cat "$ACTUAL")
if [ "$ACTUAL_TXT" != "$EXPECTED" ]; then
    echo "  [FAIL] expected '$EXPECTED', got '$ACTUAL_TXT'"
    exit 1
fi

echo "  [PASS] transitive_module_import: lib_a.greet → lib_b.shout resolves cleanly"
