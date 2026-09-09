#!/bin/sh
# Regression: `a[i]` on a bare `ptr` (e.g. a std.intarr / floatarr / longarr
# handle, which is a `void*`) must be rejected up front with a clean Aether
# diagnostic that names the fix (the accessor), instead of leaking the C
# compiler's "void value not ignored as it ought to be" through `ae run`.
# See asks/index-sugar-for-intarr-floatarr-longarr.md (the diagnostic half).
#
# A typed pointer (`*T`), a real array, and a string all index legitimately and
# must keep compiling — the diagnostic keys on element_type == NULL so it only
# fires on a bare void*.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"

fail=0

# --- 1. bare-ptr index must be rejected with the right message ---
tmpdir="$(mktemp -d)"
log="$tmpdir/cc.log"
# aetherc reports type errors on stderr; inspect the log, not the exit code.
"$AETHERC" "$SCRIPT_DIR/index_bare_ptr.ae" "$tmpdir/out.c" >"$log" 2>&1

if ! grep -q '^error' "$log"; then
    echo "  [FAIL] bare-ptr index: no error reported (the void leak is back?)"
    sed 's/^/          /' "$log" | head -6
    fail=1
elif grep -qi "void value not ignored" "$log"; then
    echo "  [FAIL] bare-ptr index: the raw C 'void value not ignored' error still leaks"
    sed 's/^/          /' "$log" | head -6
    fail=1
elif ! grep -qi "indexing is not defined for .ptr." "$log" || ! grep -q "intarr_get_unchecked" "$log"; then
    echo "  [FAIL] bare-ptr index: diagnostic doesn't name the '[]-on-ptr' rule + the accessor fix"
    sed 's/^/          /' "$log" | head -6
    fail=1
else
    echo "  [PASS] bare-ptr index rejected with a clean diagnostic"
fi
rm -rf "$tmpdir"

# --- 2. typed-pointer index must STILL compile (no over-rejection) ---
tmpdir="$(mktemp -d)"
log="$tmpdir/cc.log"
"$AETHERC" "$SCRIPT_DIR/index_typed_ptr_ok.ae" "$tmpdir/out.c" >"$log" 2>&1
if grep -q '^error' "$log"; then
    echo "  [FAIL] typed-pointer index: over-rejected (p[0] on *T should compile)"
    sed 's/^/          /' "$log" | head -6
    fail=1
else
    echo "  [PASS] typed-pointer index still compiles"
fi
rm -rf "$tmpdir"

exit $fail
