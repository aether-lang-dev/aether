#!/bin/sh
# Regression for new_defects.md section 3 — type inference of out=0 in
# a ptr-returning function. Inspects the generated C because the
# symptom is a C-level pointer truncation that only manifests at
# runtime on 64-bit platforms.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"

fail=0
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

# Case 1: ptr-returning function. Declaration of `out` with initializer
# 0 must be emitted as `void* out`, not `int out`. The int form is what
# caused the downstream subversion-port segfault.
out_c="$tmpdir/maybe_alloc.c"
if ! "$AETHERC" "$SCRIPT_DIR/maybe_alloc.ae" "$out_c" >"$tmpdir/cc1.log" 2>&1; then
    echo "  [FAIL] ptr_return_int_zero_inference: aetherc failed"
    sed 's/^/          /' "$tmpdir/cc1.log" | head -10
    exit 1
fi

body="$(awk '/^void\* maybe_alloc\(int flag\) {/,/^}/' "$out_c")"

if echo "$body" | grep -qE '^int out([^A-Za-z0-9_]|$)'; then
    echo "  [FAIL] ptr_return_int_zero_inference: out still emitted as int (pointer truncation bug)"
    echo "$body" | sed 's/^/          /'
    fail=1
elif ! echo "$body" | grep -qE '^void\* out([^A-Za-z0-9_]|$)'; then
    echo "  [FAIL] ptr_return_int_zero_inference: out not emitted as void* (unexpected shape)"
    echo "$body" | sed 's/^/          /'
    fail=1
else
    echo "  [PASS] ptr_return_int_zero_inference: out=0 widened to void* in ptr-returning fn"
fi

# Case 2: int-returning function. Widening must NOT fire — there is no
# ptr-typed assignment anywhere in the body, so `int out` is correct.
out_c2="$tmpdir/compute_int.c"
if ! "$AETHERC" "$SCRIPT_DIR/compute_int.ae" "$out_c2" >"$tmpdir/cc2.log" 2>&1; then
    echo "  [FAIL] ptr_return_int_zero_inference: aetherc failed on compute_int.ae"
    sed 's/^/          /' "$tmpdir/cc2.log" | head -10
    exit 1
fi

body2="$(awk '/^int compute\(int n\) {/,/^}/' "$out_c2")"
if ! echo "$body2" | grep -qE '^int out([^A-Za-z0-9_]|$)'; then
    echo "  [FAIL] ptr_return_int_zero_inference: int-returning fn lost int out (overzealous widening)"
    echo "$body2" | sed 's/^/          /'
    fail=1
else
    echo "  [PASS] ptr_return_int_zero_inference: int-returning fn keeps int out"
fi

exit $fail
