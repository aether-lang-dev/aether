#!/bin/sh
# #2380: std.spec indents each describe and each result line by how deeply
# its suite is nested, not by how many describes ran before it. Before the
# fix every sibling describe printed one level deeper than the last.
#
# Pinned properties, read off probe.ae's output with the colour stripped:
#   1. the three top-level siblings print flush left,
#   2. their results print one level in ("    ✓ a"),
#   3. a describe nested in a suite prints one level in, its results two,
#   4. a skip prints at its suite's depth,
#   5. the probe exits 0 (every case passed or skipped).

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"

[ -x "$AE" ] || { echo "  [SKIP] spec_sibling_describes: ae not built"; exit 0; }

cd "$ROOT" || exit 1
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT

fail() {
    echo "  [FAIL] spec_sibling_describes: $1"
    [ -f "$TMP/plain.log" ] && sed 's/^/        /' "$TMP/plain.log"
    exit 1
}

AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/probe.ae" >"$TMP/out.log" 2>&1 && probe_rc=0 || probe_rc=$?
sed 's/\x1b\[[0-9;]*m//g' "$TMP/out.log" | tr -d '\r' > "$TMP/plain.log"

[ "$probe_rc" -eq 0 ] || fail "probe exited $probe_rc"

for line in "first" "second" "third"; do
    grep -qx "$line" "$TMP/plain.log" || fail "sibling '$line' is not flush left"
done
grep -qx "    ✓ a" "$TMP/plain.log" || fail "the first suite's result is not one level in"
grep -qx "    ✓ b" "$TMP/plain.log" || fail "the second suite's result is not one level in"
grep -qx "  inner" "$TMP/plain.log" || fail "the nested describe is not one level in"
grep -qx "      ✓ c" "$TMP/plain.log" || fail "the nested suite's result is not two levels in"
grep -qx "    ⊘ d  (skipped: off)" "$TMP/plain.log" || fail "the skip is not at its suite's depth"

echo "  [PASS] spec_sibling_describes"
