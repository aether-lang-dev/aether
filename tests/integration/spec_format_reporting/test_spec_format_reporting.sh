#!/bin/sh
# Integration test for `ae test --format=<fmt>` structured reporting over
# std.spec suites.
#
# Two fixtures under fixtures/ import std.spec: math_test.ae (2 passing)
# and strings_test.ae (1 passing + 1 deliberately failing). We run
# `ae test fixtures` in each report mode and assert the aggregated
# machine-readable output:
#   --format=tap        -> one TAP v13 stream, points renumbered 1..4
#                          across both files, the failure carrying a YAML
#                          diagnostic block, plan-at-end `1..4`.
#   --format=aeocha-v1  -> one aeocha v1 block per file, comment-prefixed.
# The process exit is 1 either way (strings_test fails), which is the
# whole point: a structured run still reports pass/fail through the exit
# code.
#
# NB: no `set -e` — `ae test` in a report mode exits 1 on the failing
# fixture by design, and we check those codes explicitly.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
FIX="$SCRIPT_DIR/fixtures"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] spec_format_reporting: ae not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

fail() {
    echo "  [FAIL] spec_format_reporting: $1"
    [ -n "$2" ] && sed 's/^/    /' "$2"
    exit 1
}

# ---- TAP ------------------------------------------------------------------
AETHER_HOME="$ROOT" "$AE" test "$FIX" --format=tap >"$TMPDIR/tap.out" 2>"$TMPDIR/tap.err"
tap_rc=$?

# Exit code must reflect the failing suite.
[ "$tap_rc" -eq 1 ] || fail "tap exit code was $tap_rc, want 1" "$TMPDIR/tap.out"

head -1 "$TMPDIR/tap.out" | grep -q '^TAP version 13$' \
    || fail "tap output does not start with 'TAP version 13'" "$TMPDIR/tap.out"

# 3 passing points + 1 failing, renumbered into a single 1..4 sequence.
ok_count=$(grep -c '^ok ' "$TMPDIR/tap.out" || true)
notok_count=$(grep -c '^not ok ' "$TMPDIR/tap.out" || true)
[ "$ok_count" -eq 3 ] || fail "expected 3 'ok' points, got $ok_count" "$TMPDIR/tap.out"
[ "$notok_count" -eq 1 ] || fail "expected 1 'not ok' point, got $notok_count" "$TMPDIR/tap.out"

grep -q '^not ok 4 - mismatch fails on purpose$' "$TMPDIR/tap.out" \
    || fail "missing renumbered 'not ok 4' point" "$TMPDIR/tap.out"
grep -q 'message:.*one should equal two' "$TMPDIR/tap.out" \
    || fail "failure diagnostic message missing from TAP" "$TMPDIR/tap.out"
grep -q '^1\.\.4$' "$TMPDIR/tap.out" \
    || fail "missing or wrong plan line (want 1..4)" "$TMPDIR/tap.out"

# The child's own headers/plans must NOT leak into the aggregate stream.
tap_headers=$(grep -c '^TAP version 13$' "$TMPDIR/tap.out" || true)
[ "$tap_headers" -eq 1 ] || fail "expected exactly 1 TAP header, got $tap_headers" "$TMPDIR/tap.out"
plan_lines=$(grep -c '^1\.\.' "$TMPDIR/tap.out" || true)
[ "$plan_lines" -eq 1 ] || fail "expected exactly 1 plan line, got $plan_lines" "$TMPDIR/tap.out"

# ---- aeocha v1 ------------------------------------------------------------
AETHER_HOME="$ROOT" "$AE" test "$FIX" --format=aeocha-v1 >"$TMPDIR/v1.out" 2>"$TMPDIR/v1.err"
v1_rc=$?
[ "$v1_rc" -eq 1 ] || fail "aeocha-v1 exit code was $v1_rc, want 1" "$TMPDIR/v1.out"

# One v1 block per file, comment-prefixed.
blocks=$(grep -c '^version=1$' "$TMPDIR/v1.out" || true)
[ "$blocks" -eq 2 ] || fail "expected 2 v1 blocks, got $blocks" "$TMPDIR/v1.out"
comments=$(grep -c '^# ' "$TMPDIR/v1.out" || true)
[ "$comments" -eq 2 ] || fail "expected 2 '# <path>' headers, got $comments" "$TMPDIR/v1.out"

# The failing row is tab-packed FAIL with its message.
grep -q "$(printf 'FAIL\t2\t"mismatch fails on purpose"')" "$TMPDIR/v1.out" \
    || fail "missing tab-packed FAIL row in aeocha-v1 output" "$TMPDIR/v1.out"

# ---- unknown format is rejected ------------------------------------------
if AETHER_HOME="$ROOT" "$AE" test "$FIX" --format=bogus >"$TMPDIR/bad.out" 2>&1; then
    fail "unknown --format=bogus should have failed" "$TMPDIR/bad.out"
fi

echo "  [PASS] spec_format_reporting"
exit 0
