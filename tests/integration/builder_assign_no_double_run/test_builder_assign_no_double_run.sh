#!/bin/sh
# datastar#4: a value-returning builder with a trailing block, assigned to a
# variable, must run its body exactly ONCE.
#
# Regression guard: it used to run twice -- once through the assignment
# lowering with a NULL config, then again through the builder lowering with the
# filled one. Silent, because the second write wins so the assigned value is
# correct; only a body with side effects reveals it. The datastar port hit it
# as every SSE patch being sent twice, once without its selector.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] builder_assign_no_double_run: ae not built"; exit 0; }

OUT=$("$AE" run "$SCRIPT_DIR/prog.ae" 2>&1) || {
    echo "  [FAIL] builder_assign_no_double_run: did not build/run"
    echo "$OUT" | sed 's/^/    /' | head -12
    exit 1
}
case "$OUT" in
    *"builder assign runs once"*) ;;
    *)
        echo "  [FAIL] builder_assign_no_double_run: the body did not run exactly once"
        echo "$OUT" | sed 's/^/    /' | head -8
        exit 1 ;;
esac

# Assert the generated C carries ONE call for the assignment, not two. The
# runtime check above is the real gate; this pins the mechanism so a future
# change cannot reintroduce the duplicate call in a form that happens to be
# idempotent for this particular body.
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
"$ROOT/build/aetherc" "$SCRIPT_DIR/prog.ae" "$TMP/out.c" >/dev/null 2>&1 || {
    echo "  [FAIL] builder_assign_no_double_run: aetherc could not emit C"; exit 1; }
if grep -qE 'r = mk\([^)]*, \(void\*\)0\)' "$TMP/out.c"; then
    echo "  [FAIL] builder_assign_no_double_run: the spurious NULL-config call is back"
    grep -n "mk(" "$TMP/out.c" | sed 's/^/    /' | head -6
    exit 1
fi

echo "  [PASS] builder_assign_no_double_run: builder body runs once in assignment position"
