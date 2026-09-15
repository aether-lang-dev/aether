#!/bin/sh
# Regression: a bare `ptr` passed to a `T[]` (array) PARAMETER must be rejected
# with a clean Aether diagnostic naming the `as T[]` cast, instead of silently
# coercing and then segfaulting at runtime (the callee indexes the ptr as a
# contiguous element* and reads past the object). See
# asks/dynamic-string-array-for-sort-strings-by.md (the secondary bug), sibling
# to tests/integration/ptr_index_diagnostic (the []-on-ptr half).
#
# Positive cases: a `string[]` literal and a real `string[]` view
# (strarr.array()) must keep compiling and sorting — the check keys on the
# ARGUMENT being a bare ptr, so anything that is genuinely TYPE_ARRAY passes.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
AE="$ROOT/build/ae"

fail=0

# --- 1. bare-ptr -> string[] arg must be rejected ------------------------------
tmpdir="$(mktemp -d)"
log="$tmpdir/cc.log"
"$AETHERC" "$SCRIPT_DIR/pass_ptr_to_strings_by.ae" "$tmpdir/out.c" >"$log" 2>&1

if ! grep -q '^error' "$log"; then
    echo "  [FAIL] ptr->string[] arg: no error (the silent coerce+segfault is back?)"
    sed 's/^/          /' "$log" | head -6
    fail=1
elif ! grep -qi "bare ptr" "$log" || ! grep -qi "as string\[\]" "$log"; then
    echo "  [FAIL] ptr->string[] arg: diagnostic doesn't name 'bare ptr' + the 'as string[]' cast"
    sed 's/^/          /' "$log" | head -6
    fail=1
else
    echo "  [PASS] ptr->string[] arg rejected with a clean diagnostic"
fi
rm -rf "$tmpdir"

# --- 2. a string[] LITERAL still compiles and runs -----------------------------
if out="$("$AE" run "$SCRIPT_DIR/literal_ok.ae" 2>&1)" && \
   printf '%s' "$out" | grep -q "1.2 1.10"; then
    echo "  [PASS] string[] literal still sorts"
else
    echo "  [FAIL] string[] literal broke: $out"
    fail=1
fi

# --- 3. strarr.array() (a real string[] view) still compiles and runs ----------
if out="$("$AE" run "$SCRIPT_DIR/strarr_ok.ae" 2>&1)" && \
   printf '%s' "$out" | grep -q "1.2 1.10"; then
    echo "  [PASS] strarr.array() sorts via strings_by"
else
    echo "  [FAIL] strarr.array() broke: $out"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "PASS: ptr-to-array-arg diagnostic (reject ptr, accept literal + strarr)"
else
    echo "FAIL: ptr-to-array-arg diagnostic"
fi
exit "$fail"
