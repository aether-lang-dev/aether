#!/bin/sh
# #1576: std.spec's fluent value-comparison matchers take an optional
# trailing intent message ("why this check matters"), which prefixes the
# auto-generated failure text.
#
# This has to be an integration test rather than a spec: the thing under
# test is FAILURE TEXT, and a passing suite emits none. probe.ae fails
# every assertion on purpose; we read what it printed.
#
# Pinned properties:
#   1. a bare matcher's wording is UNCHANGED (the feature is additive —
#      this is the regression guard for ~810 existing downstream call
#      sites that pass no message),
#   2. an annotated matcher prefixes "<msg> — ",
#   3. to_equal_str switches to the caret-aligned diff once a string is
#      long enough for a quoted pair to stop being legible, and the
#      caret still lands on the first differing byte,
#   4. the message survives not_() negation,
#   5. the probe exits non-zero (the assertions really did fail — a
#      green probe would make every grep above vacuous).

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"

[ -x "$AE" ] || { echo "  [SKIP] spec_why_message: ae not built"; exit 0; }

cd "$ROOT" || exit 1
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail() {
    echo "  [FAIL] spec_why_message: $1"
    [ -f "$TMP/out.log" ] && sed 's/^/        /' "$TMP/out.log"
    exit 1
}

# The probe fails by design, so don't let `set -e` kill us on its exit.
AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/probe.ae" >"$TMP/out.log" 2>&1 && probe_rc=0 || probe_rc=$?

# Property 5 first: a probe that passed would make every check vacuous.
[ "$probe_rc" -ne 0 ] || fail "probe exited 0 — its assertions were supposed to fail"

# Strip ANSI colour so greps match the raw text.
sed 's/\x1b\[[0-9;]*m//g' "$TMP/out.log" > "$TMP/plain.log"

# Property 1: bare matchers keep their exact previous wording.
grep -q "FAIL: expected 4, got 5" "$TMP/plain.log" \
    || fail "bare int matcher's wording changed (should be 'expected 4, got 5')"
grep -q "FAIL: expected 'abd', got 'abc'" "$TMP/plain.log" \
    || fail "bare short-string matcher's wording changed"

# Property 2: annotated matchers prefix the message.
grep -q "budget is never zero — expected 4, got 5" "$TMP/plain.log" \
    || fail "int matcher did not prefix its intent message"
grep -q "name is exact — expected 'abd', got 'abc'" "$TMP/plain.log" \
    || fail "short-string matcher did not prefix its intent message"
grep -q "peer egress to internal net — 'shared' does not contain 'internal'" "$TMP/plain.log" \
    || fail "to_contain did not prefix its intent message"

# Property 3: long strings get the caret-aligned diff, message included.
# The two strings share "api.github.com" and diverge at index 14.
grep -q "egress set is the whitelist — strings differ at index 14" "$TMP/plain.log" \
    || fail "long to_equal_str did not use the caret diff form with its message"
grep -q 'expected: "api.github.com, api.anthropic.com"' "$TMP/plain.log" \
    || fail "caret diff missing the expected line"
# The caret line is spaces then a single ^; check it is present and that
# the ^ sits at the column the diff index implies (19 + d + 1 = 34).
caret_col=$(grep -n '^ *\^ *$' "$TMP/plain.log" | head -1 | cut -d: -f1)
[ -n "$caret_col" ] || fail "no caret line in the diff output"
caret_pos=$(grep '^ *\^ *$' "$TMP/plain.log" | head -1 | awk '{ print index($0, "^") }')
[ "$caret_pos" = "34" ] \
    || fail "caret at column $caret_pos, expected 34 (19 + index 14 + 1)"

# Property 4: negation keeps the message.
grep -q "must differ from the default — expected NOT 4" "$TMP/plain.log" \
    || fail "not_() dropped the intent message"

echo "  [PASS] spec_why_message: optional intent message on fluent matchers (bare wording unchanged, caret diff aligned)"
exit 0
