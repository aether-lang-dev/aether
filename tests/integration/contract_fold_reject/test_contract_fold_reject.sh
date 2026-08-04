#!/bin/sh
# Contract folding (docs/contract-folding.md): a `requires` / `where` predicate
# that is decidably FALSE at compile time — at the definition, or at a call
# site with the constant arguments substituted — is a compile error, not a
# deferred runtime panic. Each reject_*.ae MUST fail to build, for the stated
# reason (a rejection for the WRONG reason would mean the fold never ran and
# something else broke).
#
# The runtime half is asserted here too: a violation the fold CANNOT decide
# (a runtime argument) must still panic at run time with the same message
# text — folding promotes diagnoses, it must never remove checks.
#
# Pruned from the generic .ae runner (these sources exist to be compiled and
# have their diagnostics inspected).
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
if [ ! -x "$AETHERC" ]; then echo "  [SKIP] contract_fold_reject: $AETHERC not built"; exit 0; fi
TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

# $1 = source basename, $2 = extended regex the diagnostic must match
check_rejected() {
    name="$1"
    want="$2"
    if "$AETHERC" "$SCRIPT_DIR/$name.ae" "$TMPDIR/$name.c" >"$TMPDIR/$name.log" 2>&1; then
        echo "  [FAIL] contract_fold_reject: $name.ae compiled (should be rejected)"; exit 1
    fi
    if ! grep -qiE "$want" "$TMPDIR/$name.log"; then
        echo "  [FAIL] contract_fold_reject: $name.ae rejected, but not for the stated reason"
        echo "         expected a diagnostic matching: $want"
        sed 's/^/    /' "$TMPDIR/$name.log" | head -8; exit 1
    fi
}

check_rejected reject_call_const_zero  'precondition violation at compile time: b != 0 in divide'
check_rejected reject_multi_param      'precondition violation at compile time: lo <= hi in clamp'
check_rejected reject_requires_false   'requires. predicate is always false'
check_rejected reject_stale_const      'requires. predicate is always false'
check_rejected reject_enum_member      'precondition violation at compile time: n > 0'
check_rejected reject_int64_precision  'precondition violation at compile time: x == 9007199254740993'

# Runtime half: a violation folding cannot decide must still panic at run
# time. (This is the pre-existing behaviour; asserting it here pins that the
# fold did not eat the runtime check.)
cat > "$TMPDIR/rt.ae" <<'EOF'
divide(a: int, b: int where b != 0) -> int { return a / b }
main() {
    n = 0
    x = divide(10, n)
    println("${x}")
}
EOF
if [ -x "$AE" ]; then
    if "$AE" run "$TMPDIR/rt.ae" >"$TMPDIR/rt.log" 2>&1; then
        echo "  [FAIL] contract_fold_reject: runtime violation did not panic"; exit 1
    fi
    if ! grep -q "precondition_violation: b != 0 in divide" "$TMPDIR/rt.log"; then
        echo "  [FAIL] contract_fold_reject: runtime panic lost its message"
        sed 's/^/    /' "$TMPDIR/rt.log" | head -6; exit 1
    fi
fi

echo "  [PASS] contract_fold_reject: const-arg call / multi-param / requires-false / stale-const / enum / int64-precision all rejected; runtime check intact"
exit 0
