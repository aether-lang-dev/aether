#!/bin/sh
# Regression for #1097: a selective `import std.tcp (connect)` in the top-level
# unit must NOT suppress instantiation of the tuple wrappers (poll2/read_n/
# write_n) that an imported library uses transitively.
#
# Before the fix the transitive cross-module merge skipped std.tcp entirely
# once it was a *direct* (selective) import, so the omitted wrappers were never
# cloned and relaylib's call sites degraded to an undefined `tcp_poll2`. A build
# with NO top-level std.tcp import already worked (std.tcp reached only
# transitively → full merge), so a partial import was wrongly *stronger* than no
# import. This test pins the "partial import" case to build.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

# Run from the test dir so `import relaylib` resolves against ./relaylib/.
cd "$SCRIPT_DIR"

ACTUAL="$TMPDIR/actual.txt"
if ! AETHER_HOME="$ROOT" "$AE" run main.ae >"$ACTUAL" 2>"$TMPDIR/err.log"; then
    echo "  [FAIL] ae run main.ae exited non-zero — selective import suppressed a"
    echo "         transitively-used std.tcp wrapper (regression of #1097)"
    head -15 "$TMPDIR/err.log"
    exit 1
fi

ACTUAL_TXT=$(cat "$ACTUAL")
if [ "$ACTUAL_TXT" != "ok" ]; then
    echo "  [FAIL] expected 'ok', got '$ACTUAL_TXT'"
    exit 1
fi

echo "  [PASS] issue1097: selective std.tcp import doesn't suppress transitive wrapper instantiation"
