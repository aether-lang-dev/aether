#!/bin/sh
# Issue #1682: `ae test` collected at most 256 files and printed that as the
# suite total, so a repository with more tests than that reported a full pass
# over a run that never opened the rest. In this repo that was 256 of 593.
#
# The count is checked through `ae test --list`, which does discovery and
# nothing else: compiling 300 programs to prove a discovery bound would trade
# the thing being measured for several minutes of build time. A separate small
# suite is actually run, so the collected list is known to be what executes.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] test_discovery_no_cap: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

# 300 files: comfortably past the old 256 ceiling, and past it by enough that
# an off-by-one in a replacement bound would still show.
mkdir -p "$TMP/tests"
i=1
while [ "$i" -le 300 ]; do
    printf 'main() {\n    println("t%s")\n}\n' "$i" > "$TMP/tests/test_$i.ae"
    i=$((i + 1))
done

# A fixture is input to a test, not a test: this one fails on purpose, the way
# the spec reporter's fixtures do.
mkdir -p "$TMP/tests/fixtures"
printf 'main() {\n    exit(1)\n}\n' > "$TMP/tests/fixtures/deliberate_test.ae"

listed="$("$AE" test --list "$TMP" 2>/dev/null | wc -l | tr -d ' ')"
[ "$listed" = "300" ] \
    || fail "discovery found $listed of 300 test files (the 256 ceiling is back, or fixtures leaked in)"

"$AE" test --list "$TMP" 2>/dev/null | grep -q "/fixtures/" \
    && fail "a file under fixtures/ was collected as a test"

# The list is what runs: a small suite executes, and its total is the count.
mkdir -p "$TMP/small/tests"
printf 'main() {\n    println("ok")\n}\n' > "$TMP/small/tests/test_one.ae"
printf 'main() {\n    println("ok")\n}\n' > "$TMP/small/tests/test_two.ae"
out="$("$AE" test "$TMP/small" 2>&1)" || fail "a passing two-test suite exited non-zero: $out"
printf '%s' "$out" | grep -q "2 passed, 0 failed, 2 total" \
    || fail "expected '2 passed, 0 failed, 2 total', got: $(printf '%s' "$out" | tail -1)"

echo "  [PASS] test_discovery_no_cap: discovery has no ceiling and skips fixtures"
