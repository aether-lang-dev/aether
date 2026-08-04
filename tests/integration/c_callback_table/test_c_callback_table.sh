#!/bin/sh
# Integration test for Aether functions in C callback tables (#1240).
#
# Regression for a silent miscompile: assigning an Aether function into a
# C-owned struct's function-pointer field stored an `_AeClosure` BOX there. That
# box is a heap pointer, so the first callback from C jumped into a malloc'd
# struct and took SIGBUS. It compiled without a warning, and the `fn(...)`-typed
# spelling of the same field was rejected outright at typecheck, so there was no
# correct way to write it.
#
# Asserts:
#   1. the table builds with `fn(...)`-typed fields and C calls back correctly;
#   2. codegen stores the raw function symbol, not a closure box;
#   3. a wrong-signature function in a typed slot is a compile error.
#
# 3 is what makes the slot typed rather than merely pointer-shaped.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] c_callback_table: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"; rm -f "$SCRIPT_DIR/bad_sig.ae"' EXIT

cd "$SCRIPT_DIR" || exit 1

# (1) builds, and C calls back into both Aether functions.
if ! "$AE" build probe.ae -o "$TMP/probe" >"$TMP/build.log" 2>&1; then
    echo "  [FAIL] c_callback_table: probe did not build"
    sed 's/^/        /' "$TMP/build.log" | head -15
    exit 1
fi
out=$("$TMP/probe" 2>&1)
if [ "$out" != "ok" ]; then
    echo "  [FAIL] c_callback_table: probe printed '$out', want 'ok'"
    exit 1
fi

# (2) the field holds the function's address, not a boxed closure.
"$ROOT/build/aetherc" probe.ae "$TMP/probe.gen.c" >/dev/null 2>&1
# Match an ASSIGNMENT into the fields, not the prelude's unconditional
# definition of the _aether_box_closure helper itself.
if grep -qE '(hashFunction|keyCompare) = _aether_box_closure' "$TMP/probe.gen.c"; then
    echo "  [FAIL] c_callback_table: a closure box is still stored in a C struct field"
    grep -nE '(hashFunction|keyCompare) = ' "$TMP/probe.gen.c" | head -5 | sed 's/^/        /'
    exit 1
fi
if ! grep -q 'hashFunction = (__typeof__' "$TMP/probe.gen.c"; then
    echo "  [FAIL] c_callback_table: expected the raw symbol cast to the header's field type"
    grep -n 'hashFunction' "$TMP/probe.gen.c" | head -5 | sed 's/^/        /'
    exit 1
fi

# (3) a wrong signature must not typecheck into a typed slot.
sed 's/^my_hash(key: ptr) -> int {/my_hash(key: ptr, extra: ptr) -> int {/' probe.ae > bad_sig.ae
if "$AE" build bad_sig.ae -o "$TMP/bad" >"$TMP/bad.log" 2>&1; then
    echo "  [FAIL] c_callback_table: a wrong-signature function was accepted in a typed slot"
    exit 1
fi

echo "  [PASS] c_callback_table: Aether functions callable from C through typed table fields"
