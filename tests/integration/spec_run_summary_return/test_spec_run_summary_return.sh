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
#   - a failing suite still exits 1, THROUGH the return  (run_summary now
#     returns 1 rather than exit(1)-ing, and every caller was converted to
#     propagate it; a caller that drops the value turns a failing suite green,
#     which is what the last case here guards)
#   - a passing suite still exits 0
#   - cleanup after the call runs on the FAILURE path too (what returning
#     instead of exiting buys: exit(1) skipped it on exactly the runs where a
#     leaked arena or an orphaned listener matters most)

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
    return spec.run_summary(fw)
}
AEEOF
if "$AE" run "$TMP/failing.ae" >"$TMP/f.out" 2>&1; then
    sed 's/^/         /' "$TMP/f.out"
    fail "a failing suite exited 0"
fi

# --- cleanup runs on the FAILURE path too ---------------------------------
# The point of returning rather than exiting. Under exit(1) everything after
# the call was skipped on a failing run -- arena.destroy, http.server_stop,
# sqlite.close -- which is precisely when a leaked arena or an orphaned
# listener does damage.
cat > "$TMP/failing_cleanup.ae" <<'AEEOF'
import std.spec
main() {
    fw = spec.init()
    spec.describe(fw, "arithmetic") {
        spec.it("is deliberately wrong") callback {
            spec.assert_eq(1 + 1, 3, "1+1 == 3")
        }
    }
    _rc = spec.run_summary(fw)
    println("CLEANUP-ON-FAILURE")
    return _rc
}
AEEOF
if "$AE" run "$TMP/failing_cleanup.ae" >"$TMP/fc.out" 2>&1; then
    sed 's/^/         /' "$TMP/fc.out"
    fail "a failing suite with trailing cleanup exited 0"
fi
grep -q "CLEANUP-ON-FAILURE" "$TMP/fc.out" \
    || { sed 's/^/         /' "$TMP/fc.out"
         fail "cleanup after run_summary was skipped on the failure path"; }

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
    return spec.run_summary(fw)
}
AEEOF
"$AE" run "$TMP/passing.ae" >"$TMP/p.out" 2>&1 \
    || { sed 's/^/         /' "$TMP/p.out"; fail "a passing suite exited non-zero"; }

echo "  [PASS] spec_run_summary_return: returns 0 captured, continues past the call, failure propagates through the return, cleanup runs on both paths"
