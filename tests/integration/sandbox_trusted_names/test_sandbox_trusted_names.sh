#!/bin/sh
# Trusted names in sandbox.enforce(perms, foo, ...): the cases that need a
# module of their own (trusting a whole module) and the ones that must not
# compile. The runtime behaviour of trusted functions is in
# std/sandbox/test_sandbox_trust.ae.
#
# Each reject case checks for its own diagnostic, not just a failed build: a
# fixture that broke for an unrelated reason must not count as a pass.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
FIX="$SCRIPT_DIR/fixtures"
AETHERC="$ROOT/build/aetherc"
AE="$ROOT/build/ae"

pass=0
fail=0
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

ok() { echo "  [PASS] $1"; pass=$((pass + 1)); }
bad() { echo "  [FAIL] $1"; fail=$((fail + 1)); }

# 1. A trusted module covers every call into it written in the block.
out="$(cd "$FIX/module_prefix" && AETHER_HOME="$ROOT" "$AE" run main.ae 2>&1)"
expected="direct: true
audit.secret: s3cret
audit.also_secret: s3cret"
if [ "$out" = "$expected" ]; then
    ok "trusting a module covers its calls; the rest of the block stays sandboxed"
else
    bad "module trust"
    printf '%s\n' "$out" | sed 's/^/        /'
fi

# 2. Misuse is a compile error with a diagnostic that says what is wrong.
reject() {
    name="$1"; needle="$2"
    msg="$(cd "$FIX/reject" && "$AETHERC" "$name.ae" "$tmp/$name.c" 2>&1)"
    rc=$?
    if [ $rc -ne 0 ] && printf '%s' "$msg" | grep -qF "$needle"; then
        ok "$name is rejected: $needle"
    else
        bad "$name should fail with: $needle"
        printf '%s\n' "$msg" | sed 's/^/        /' | head -8
    fi
}

reject value_use "can only be called, not used as a value"
reject unknown_name "'peak' is not a function or an imported module"
reject never_called "the block never calls it"
reject not_a_name "must be the name of a function or an imported module"
reject block_not_at_call "needs its block written at the call"
reject extern_via_module "'os.getenv' is a C extern"
reject extern_by_name "cannot trust 'my_c_thing': it is a C extern"

# The not-at-the-call case reports exactly one error, not that plus an
# arity complaint about the same call.
n="$(cd "$FIX/reject" && "$AETHERC" block_not_at_call.ae "$tmp/x.c" 2>&1 | grep -c '^error')"
if [ "$n" = "1" ]; then
    ok "a misplaced block gives one error, not a follow-on arity error"
else
    bad "a misplaced block gave $n errors"
fi

echo ""
echo "sandbox trusted names: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
