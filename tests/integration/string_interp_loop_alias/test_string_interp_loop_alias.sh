#!/bin/sh
# Issue #250 regression: string interpolation in accumulator loops
# where the LHS appears on the RHS — `out = "${out}…"`.
#
# Earlier, the codegen recycled the LHS's backing buffer between
# iterations, so iteration N+1 saw freed-then-reallocated bytes
# before the new content was written. Symptom: garbage prefix,
# correct suffix (e.g. "!d^?U### Request headers...").
#
# This test runs the program and compares stdout against a known-good
# expected output. A regression that produces garbage bytes shows up
# immediately as a diff. Test is portable: works on every host where
# `ae run` works (no ASAN required).

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

SRC="$SCRIPT_DIR/loop_self_aliasing.ae"
EXPECTED="$SCRIPT_DIR/expected_output.txt"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

ACTUAL="$TMPDIR/actual.txt"
if ! AETHER_HOME="$ROOT" "$AE" run "$SRC" >"$ACTUAL" 2>"$TMPDIR/err.log"; then
    echo "  [FAIL] ae run exited non-zero"
    cat "$TMPDIR/err.log"
    exit 1
fi

# Compare stdout against the recorded expected output. The runtime
# emits LF on every platform (Windows main() puts stdout in binary
# mode), so a plain shell string compare suffices — no diff/cmp
# dependency, which the CI Windows shell does not ship with.
ACTUAL_TXT="$(cat "$ACTUAL")"
EXPECTED_TXT="$(cat "$EXPECTED")"
if [ "$ACTUAL_TXT" != "$EXPECTED_TXT" ]; then
    echo "  [FAIL] string-interp accumulator loops produced wrong output"
    echo "  --- expected ---"
    printf '%s\n' "$EXPECTED_TXT"
    echo "  --- actual ---"
    printf '%s\n' "$ACTUAL_TXT"
    exit 1
fi

echo "  [PASS] string_interp_loop_alias: 5 accumulator-loop shapes round-trip clean"
