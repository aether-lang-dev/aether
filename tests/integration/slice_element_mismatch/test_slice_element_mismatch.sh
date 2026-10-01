#!/bin/sh
# #2330: an array or slice whose element type differs from a slice slot's is
# a compile error, not a reinterpretation of its memory.
#
# An int[3] passed as a long[] was read as three 8-byte longs out of 12
# bytes; as a byte[], as the first three bytes of the first int. Element
# compatibility is right for a value and wrong for a view. An array literal
# is converted instead (tests/regression/test_issue2330_slice_element_width).
# The cases here must not compile, and must say why.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT
fail=0

expect_error() {
    if AETHER_HOME="" "$AE" build "$TMPDIR/$1.ae" -o "$TMPDIR/$1" >"$TMPDIR/$1.log" 2>&1; then
        echo "  [FAIL] slice_element_mismatch: $1 compiled"
        fail=1
        return
    fi
    if ! grep -q "cannot view $2 elements as $3\[\]" "$TMPDIR/$1.log"; then
        echo "  [FAIL] slice_element_mismatch: $1 did not name $2 viewed as $3[]"
        head -8 "$TMPDIR/$1.log" | sed 's/^/          /'
        fail=1
    fi
}

cat >"$TMPDIR/int_as_long.ae" <<'AE'
g(s: long[]) -> long { return s[0] }
main() {
    a = [1, 2, 3]
    println("${g(a)}")
}
AE
expect_error int_as_long int long

cat >"$TMPDIR/int_as_byte.ae" <<'AE'
h(s: byte[]) -> int { return s[0] }
main() {
    a = [1, 2, 3]
    println("${h(a)}")
}
AE
expect_error int_as_byte int byte

cat >"$TMPDIR/slice_as_long.ae" <<'AE'
g(s: long[]) -> long { return s[0] }
pass(s: int[]) -> long { return g(s) }
main() {
    a = [1, 2, 3]
    println("${pass(a)}")
}
AE
expect_error slice_as_long int long

[ "$fail" -eq 0 ] || exit 1
echo "  [PASS] slice_element_mismatch: an array or slice of another element type is refused, by name"
exit 0
