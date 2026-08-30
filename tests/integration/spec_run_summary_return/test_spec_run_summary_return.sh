#!/bin/sh
# std.spec: run_summary's return VALUE, and the exit status, are separate
# guarantees -- and only the second was ever tested.
#
# run_summary exit(1)s on failure but used to fall off the end on success.
# Run for its process status that looked fine (the fall-through still exits
# 0), so every existing test passed. Captured as a value it was garbage:
# measured at 24 for an all-green suite, which made aeb's fan-out
# orchestrator -- it rewrites each test to `<fn>(s: ptr) -> int` and uses the
# return as the node's result -- report 132 passing suites as FAILED.
# (asks/spec-run-summary-should-exit-0-on-success.md)
#
# Asserts, in the order they matter:
#   - an all-green suite RETURNS 0 when captured        (the reported bug)
#   - execution continues past run_summary on success   (the ask proposed
#     exit(0) here; 17 callers in this tree do real cleanup after the call,
#     so exiting would silently skip arena.destroy / server_stop / close)
#   - a failing suite still exits 1                     (unchanged contract:
#     all 55 callers use a bare call whose value is discarded, so the failure
#     path must exit rather than return, or CI goes green on real failures)
#   - a passing suite still exits 0

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] spec_run_summary_return: ae not built"; exit 0; }

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP" || :; return 0; }
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

# --- the captured return value, and continuation past the call ------------
OUT=$("$AE" run "$SCRIPT_DIR/probe.ae" 2>&1) || {
    echo "$OUT" | sed 's/^/         /'
    fail "the capture probe exited non-zero"
}
case "$OUT" in
    *"PASS: run_summary returned 0"*) ;;
    *) echo "$OUT" | sed 's/^/         /'
       fail "an all-green suite did not return 0 when captured" ;;
esac
case "$OUT" in
    *"CLEANUP-REACHED"*) ;;
    *) echo "$OUT" | sed 's/^/         /'
       fail "execution did not continue past run_summary on the success path" ;;
esac

# --- a failing suite must still exit 1 ------------------------------------
# The contract every caller in the tree depends on. `return 1` here instead of
# exit(1) would make all 55 of them exit 0 on failure, because each ends in a
# bare `spec.run_summary(fw)` whose value is discarded -- verified, and the
# reason the failure path was left alone.
cat > "$TMP/failing.ae" <<'AEEOF'
import std.spec
main() {
    fw = spec.init()
    spec.describe(fw, "arithmetic") {
        spec.it("is deliberately wrong") callback {
            spec.assert_eq(1 + 1, 3, "1+1 == 3")
        }
    }
    spec.run_summary(fw)
}
AEEOF
if "$AE" run "$TMP/failing.ae" >"$TMP/f.out" 2>&1; then
    sed 's/^/         /' "$TMP/f.out"
    fail "a failing suite exited 0"
fi

# --- and a passing suite must still exit 0 --------------------------------
cat > "$TMP/passing.ae" <<'AEEOF'
import std.spec
main() {
    fw = spec.init()
    spec.describe(fw, "arithmetic") {
        spec.it("is right") callback {
            spec.assert_eq(1 + 1, 2, "1+1 == 2")
        }
    }
    spec.run_summary(fw)
}
AEEOF
"$AE" run "$TMP/passing.ae" >"$TMP/p.out" 2>&1 \
    || { sed 's/^/         /' "$TMP/p.out"; fail "a passing suite exited non-zero"; }

echo "  [PASS] spec_run_summary_return: returns 0 captured, continues past the call, exit codes unchanged"
