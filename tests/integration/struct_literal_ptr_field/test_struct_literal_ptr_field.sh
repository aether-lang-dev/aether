#!/bin/sh
# datastar#10: a `*T` field read inside a struct literal must emit `->`.
#
# Regression guard: this used to emit `c.field` and fail in GCC, with the error
# pointing at generated C the user never wrote.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] struct_literal_ptr_field: ae not built"; exit 0; }

OUT=$("$AE" run "$SCRIPT_DIR/prog.ae" 2>&1) || {
    echo "  [FAIL] struct_literal_ptr_field: did not build/run"
    echo "$OUT" | sed 's/^/    /' | head -12
    exit 1
}
case "$OUT" in
    *"struct literal ptr field ok"*) ;;
    *) echo "  [FAIL] struct_literal_ptr_field: unexpected output"; echo "$OUT" | sed 's/^/    /' | head -8; exit 1 ;;
esac

# Assert the ACCESSOR directly, not just that it runs: a future change could
# make this compile by some other route and still be wrong.
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
"$ROOT/build/aetherc" "$SCRIPT_DIR/prog.ae" "$TMP/out.c" >/dev/null 2>&1 || {
    echo "  [FAIL] struct_literal_ptr_field: aetherc could not emit C"; exit 1; }
if grep -qE '\.code = c\.auth_code' "$TMP/out.c"; then
    echo "  [FAIL] struct_literal_ptr_field: emitted 'c.auth_code' in a struct literal"
    exit 1
fi
grep -qE '\.code = c->auth_code' "$TMP/out.c" || {
    echo "  [FAIL] struct_literal_ptr_field: expected 'c->auth_code' in the literal"
    grep -n "auth_code" "$TMP/out.c" | sed 's/^/    /' | head -6
    exit 1; }

echo "  [PASS] struct_literal_ptr_field: pointer deref in a struct literal emits ->"
