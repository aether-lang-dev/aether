#!/bin/sh
# Regression for #1778: a tuple-returning function called through an `fn`
# parameter.
#
#  (1) A BARE `fn` carries no signature, so a call through it is typed `void`
#      and the tuple return is erased. This used to fail with a generic
#      "not a tuple" at the destructure PLUS misleading "Undefined variable"
#      follow-ons attributed to line 1. Now: one targeted error that names the
#      bare-fn cause and the signatured-fn fix, and NO line-1 cascade.
#  (2) The SIGNATURED form `fn(int,int) -> (int,string)` preserves the tuple
#      and runs.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
AE="$ROOT/build/ae"

fail=0

# (1) bare fn: targeted diagnostic, and no misleading line-1 "Undefined variable".
tmpdir="$(mktemp -d)"
log="$tmpdir/cc.log"
"$AETHERC" "$SCRIPT_DIR/bare_fn.ae" "$tmpdir/out.c" >"$log" 2>&1
if ! grep -qi "bare .fn. parameter loses its return type" "$log"; then
    echo "  [FAIL] bare-fn destructure did not give the signatured-fn hint"
    sed 's/^/        /' "$log" | head -4
    fail=1
elif grep -qi "Undefined variable" "$log"; then
    echo "  [FAIL] the misleading 'Undefined variable' cascade is still present"
    sed 's/^/        /' "$log" | head -6
    fail=1
else
    echo "  [PASS] bare-fn destructure: targeted hint, no Undefined-variable cascade"
fi
rm -rf "$tmpdir"

# (2) signatured fn: compiles and runs, tuple preserved.
out="$("$AE" run "$SCRIPT_DIR/signatured_fn.ae" 2>&1)"
if printf '%s' "$out" | grep -q "via fn: 5"; then
    echo "  [PASS] signatured fn preserves the tuple return"
else
    echo "  [FAIL] signatured fn: got '$out', expected 'via fn: 5 ...'"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "PASS: #1778 tuple-through-fn"
    exit 0
fi
echo "FAIL: #1778"
exit 1
