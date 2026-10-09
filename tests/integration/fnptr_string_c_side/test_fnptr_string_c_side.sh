#!/bin/sh
# #2586: the C side of the typed fn pointer convention.
#
# A string returned through a typed fn pointer (`fn(...) -> string`) is the
# caller's only when an Aether function handed it over (it marks the string
# it returns); a C function's string is copied by the call and stays C's.
# c_pointer.ae sends C-made pointers (cast from a raw `ptr`, a raw `ptr`
# passed for a typed parameter, an extern named as a value, a struct laid
# over C memory, one an extern returns, one C filled, one C passes to an
# Aether callback) and Aether functions through the same typed parameters,
# three rounds and a heap window; freeing C's string crashed. raw_to_c.ae
# hands an Aether function to C by name, through a typed local and through a
# typed parameter: C reads its result as text, not an Aether string header.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] fnptr_string_c_side: $AE not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

run_case() {
    name="$1"
    if ! AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/$name.ae" -o "$TMPDIR/$name" \
            --extra "$SCRIPT_DIR/shim.c" >"$TMPDIR/$name.build.log" 2>&1; then
        echo "  [FAIL] fnptr_string_c_side: $name did not build"
        sed 's/^/    /' "$TMPDIR/$name.build.log" | head -15
        exit 1
    fi
    if ! "$TMPDIR/$name" >"$TMPDIR/$name.log" 2>&1; then
        echo "  [FAIL] fnptr_string_c_side: $name exited non-zero"
        sed 's/^/    /' "$TMPDIR/$name.log" | head -20
        exit 1
    fi
}

run_case c_pointer
if ! grep -q "^c_pointer done$" "$TMPDIR/c_pointer.log"; then
    echo "  [FAIL] fnptr_string_c_side: C's strings through typed pointers"
    sed 's/^/    /' "$TMPDIR/c_pointer.log" | head -20
    exit 1
fi

run_case raw_to_c
if [ "$(grep -c '^len=12$' "$TMPDIR/raw_to_c.log")" -ne 4 ]; then
    echo "  [FAIL] fnptr_string_c_side: C read an Aether function's string wrong"
    sed 's/^/    /' "$TMPDIR/raw_to_c.log" | head -10
    exit 1
fi

echo "  [PASS] fnptr_string_c_side: a C pointer's string stays C's, and C reads an Aether function's text"
exit 0
