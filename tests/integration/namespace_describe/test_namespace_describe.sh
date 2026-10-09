#!/bin/sh
# Verify aetherc --emit-namespace-manifest produces the expected JSON
# AND aetherc --emit-namespace-describe produces a self-contained C
# stub that compiles. These are the two surfaces `ae build --namespace`
# leans on; tested here in isolation so a regression is easy to localize.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT
pass=0; fail=0

# Step 1: JSON emitter sees every field.
JSON="$(AETHER_HOME="" "$ROOT/build/aetherc" --emit-namespace-manifest "$SCRIPT_DIR/manifest.ae" /dev/null 2>"$TMPDIR/json.err")"
if [ -z "$JSON" ]; then
    echo "  [FAIL] --emit-namespace-manifest produced no output"
    cat "$TMPDIR/json.err"
    fail=$((fail + 1))
else
    if echo "$JSON" | grep -q '"namespace": "describe_test"'; then
        echo "  [PASS] manifest JSON contains namespace name"; pass=$((pass + 1))
    else
        echo "  [FAIL] manifest JSON missing namespace name"
        echo "$JSON"
        fail=$((fail + 1))
    fi
    # Inputs in declaration order
    if echo "$JSON" | grep -q '"alpha".*"beta".*"gamma"'; then
        echo "  [PASS] inputs in declaration order"; pass=$((pass + 1))
    else
        echo "  [FAIL] inputs out of order or missing"
        fail=$((fail + 1))
    fi
    # Function-typed input survives
    if echo "$JSON" | grep -q '"fn(int) -> int"'; then
        echo "  [PASS] function-typed input preserved"; pass=$((pass + 1))
    else
        echo "  [FAIL] function-typed input lost"
        fail=$((fail + 1))
    fi
    # All three binding targets
    if echo "$JSON" | grep -q '"DescribeTest"' && \
       echo "$JSON" | grep -q '"dt_module"' && \
       echo "$JSON" | grep -q '"dt"'; then
        echo "  [PASS] all three binding targets present"; pass=$((pass + 1))
    else
        echo "  [FAIL] missing binding entries"
        fail=$((fail + 1))
    fi
fi

# Step 2: C-stub emitter produces compilable code.
DESCRIBE_C="$TMPDIR/describe.c"
if ! AETHER_HOME="" "$ROOT/build/aetherc" --emit-namespace-describe "$SCRIPT_DIR/manifest.ae" "$DESCRIBE_C" 2>"$TMPDIR/desc.err"; then
    echo "  [FAIL] --emit-namespace-describe failed"; cat "$TMPDIR/desc.err"; fail=$((fail + 1))
elif ! gcc -c -fPIC "$DESCRIBE_C" -o "$TMPDIR/describe.o" 2>"$TMPDIR/gcc.err"; then
    echo "  [FAIL] generated describe.c does not compile"; cat "$TMPDIR/gcc.err"; fail=$((fail + 1))
else
    echo "  [PASS] generated describe.c compiles standalone"; pass=$((pass + 1))
fi

echo ""
echo "namespace_describe: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
