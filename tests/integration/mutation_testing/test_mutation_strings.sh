#!/bin/sh
# Regression for std.mutation's STRING-literal mutation + the
# operator-in-string skip (string-boundary awareness).
#
# fixture_str/sut.ae has three string literals and one of them (help)
# contains an arithmetic operator INSIDE the string ("a + b"):
#   - name()  is tested        -> STR->EMPTY killed
#   - motto() is NOT tested     -> STR->EMPTY survives
#   - help()  is NOT tested     -> STR->EMPTY survives
# and there are NO code-level arithmetic operators, so the run must be
# exactly "1/3 ... 33%" with two STR->EMPTY survivors.
#
# Asserts:
#   1. baseline passes,
#   2. exact score "1/3 ... 33%",
#   3. a STR->EMPTY survivor is reported (string mutation works),
#   4. NO ADD->SUB mutant appears — the ` + ` inside help()'s string was
#      skipped (the boundary-awareness / false-mutant fix),
#   5. SUT restored byte-identical.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
SUT="$SCRIPT_DIR/fixture_str/sut.ae"
TEST="$SCRIPT_DIR/fixture_str/sut_test.ae"

if [ "$OS" = "Windows_NT" ]; then
    echo "  [SKIP] mutation_testing_strings: driver shells out via POSIX rm/ae"
    exit 0
fi
if [ ! -x "$AE" ]; then
    echo "  [SKIP] mutation_testing_strings: ae not built"
    exit 0
fi
if command -v md5sum >/dev/null; then MD5="md5sum"; else MD5="cksum"; fi

cd "$ROOT" || exit 1
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

fail() {
    echo "  [FAIL] mutation_testing_strings — $1"
    [ -f "$TMPDIR/out.log" ] && tail -40 "$TMPDIR/out.log" | sed 's/^/    /'
    exit 1
}

SUT_BEFORE="$($MD5 "$SUT" | awk '{print $1}')"

rm -rf "$HOME/.aether/cache"
# AE_BIN points the driver's per-mutant sub-builds at the in-tree ae;
# AETHER_HOME resolves std.*; lib_dir arg is where `import sut` resolves.
if ! AETHER_HOME="$ROOT" AE_BIN="$AE" \
        "$AE" run "$ROOT/examples/mutation-testing/mutate.ae" -- \
        "$SUT" "$TEST" "$SCRIPT_DIR/fixture_str" >"$TMPDIR/out.log" 2>&1; then
    fail "driver exited non-zero"
fi

SUT_AFTER="$($MD5 "$SUT" | awk '{print $1}')"
[ "$SUT_BEFORE" = "$SUT_AFTER" ] || fail "SUT not restored (md5 $SUT_BEFORE -> $SUT_AFTER)"

grep -q "baseline: suite passes" "$TMPDIR/out.log" || fail "no baseline-pass line"
grep -q "1/3 mutants killed — mutation score 33%" "$TMPDIR/out.log" \
    || fail "unexpected score (wanted 1/3, 33%)"
# Survivors/kill are anchored to source lines: name() killed on line 10,
# motto() survives on line 12.
grep -Eq "SURVIVED +sut\.ae:12 +STR->EMPTY" "$TMPDIR/out.log" \
    || fail "no STR->EMPTY survivor at sut.ae:12 (string mutation broken?)"
grep -Eq "killed +sut\.ae:10 +STR->EMPTY" "$TMPDIR/out.log" \
    || fail "STR->EMPTY not killed at sut.ae:10"
# The crucial boundary-awareness check: the ` + ` inside help()'s string
# must NOT have produced an operator mutant.
if grep -q "ADD->SUB" "$TMPDIR/out.log"; then
    fail "ADD->SUB appeared — operator inside a string literal was NOT skipped"
fi

echo "  [PASS] mutation_testing_strings"
exit 0
