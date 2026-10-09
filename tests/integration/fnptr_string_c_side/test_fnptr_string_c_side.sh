#!/bin/sh
# #2586: the C side of the typed fn pointer convention.
#
# A string returned through a typed fn pointer (`fn(...) -> string`) is the
# caller's when every such pointer in the program comes from Aether: a named
# function used as the value hands its result over owned, and the caller
# frees it. A pointer made from C (here a `ptr` from C cast with `as fn`)
# returns C's string, so a program holding one keeps the borrowed reading;
# freeing it crashed. And an Aether function handed to C as a plain pointer
# stays the function itself, so C reads its result as text, not an Aether
# string header.

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
if [ "$(grep -c '^positive$' "$TMPDIR/c_pointer.log")" -ne 3 ] ||
   [ "$(grep -c '^other$' "$TMPDIR/c_pointer.log")" -ne 3 ] ||
   ! grep -q "c_pointer done" "$TMPDIR/c_pointer.log"; then
    echo "  [FAIL] fnptr_string_c_side: a C function's string read through a typed pointer"
    sed 's/^/    /' "$TMPDIR/c_pointer.log" | head -20
    exit 1
fi

run_case raw_to_c
if ! grep -q "^len=12$" "$TMPDIR/raw_to_c.log"; then
    echo "  [FAIL] fnptr_string_c_side: C read an Aether function's string wrong"
    sed 's/^/    /' "$TMPDIR/raw_to_c.log" | head -10
    exit 1
fi

echo "  [PASS] fnptr_string_c_side: a C pointer's string stays C's, and C reads an Aether function's text"
exit 0
