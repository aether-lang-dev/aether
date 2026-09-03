#!/bin/sh
# #1877: a typed pointer (`*T`) must be assignable to a bare `ptr`, which is
# the universal pointer exactly as `void*` is in C.
#
# Regression guard for the E0200 false positive that blocked the native
# WebDriver binding: std.cryptography's tls13_cert assigns a `*LeafCert` into a
# slot inferred as bare `ptr`, so ANY program whose import graph reached that
# function failed to typecheck with an un-actionable post-inlining line number.
#
# Also asserts the limit of the rule: two different TYPED pointers are still
# incompatible, so this widens only to and from `ptr`.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
TMP="${TMPDIR:-/tmp}/ae_ptr_widen_$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# 1. The widening must compile AND run.
OUT=$("$AE" run "$SCRIPT_DIR/prog.ae" 2>&1) || {
    echo "  [FAIL] ptr_widen_typed_to_bare: *T -> ptr was rejected"
    echo "$OUT" | sed 's/^/    /' | head -10
    exit 1
}
case "$OUT" in
    *"ptr widening ok"*) ;;
    *)
        echo "  [FAIL] ptr_widen_typed_to_bare: unexpected output"
        echo "$OUT" | sed 's/^/    /' | head -10
        exit 1
        ;;
esac

# 2. The rule must NOT make unrelated typed pointers interchangeable.
# Written inline rather than kept as a .ae file: the test sweep compiles every
# .ae under tests/ standalone, and a deliberate counter-example would fail it.
cat > "$TMP/reject_mismatched.ae" << 'BADEOF'
struct Leaf { a: int }
struct Other { b: int }

main() {
    f = null as *Leaf
    let g: *Other = f     // *Leaf -> *Other: must stay an error
    println("should not compile")
    return 0
}
BADEOF
if "$AE" build "$TMP/reject_mismatched.ae" -o "$TMP/bad" >"$TMP/log" 2>&1; then
    echo "  [FAIL] ptr_widen_typed_to_bare: *Leaf -> *Other was accepted;"
    echo "         the widening must apply only to the untyped ptr"
    exit 1
fi
grep -q 'E0200' "$TMP/log" || {
    echo "  [FAIL] ptr_widen_typed_to_bare: *Leaf -> *Other failed, but not with E0200"
    sed 's/^/    /' "$TMP/log" | head -6
    exit 1
}

echo "  [PASS] ptr_widen_typed_to_bare: *T widens to ptr; mismatched typed pointers still rejected"
