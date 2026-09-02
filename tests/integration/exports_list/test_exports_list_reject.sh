#!/bin/sh
# Negative test: names NOT in the `exports (…)` list must be rejected
# at qualified-call sites. Compilation should FAIL with E0301 (undefined
# function) or similar — the listed name `square` is fine; the
# non-listed `_internal_helper` and `SECRET` are blocked.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

pass=0
fail=0

# Test 1: calling a non-listed function should fail to resolve.
cat > /tmp/ae_exports_list_bad1.ae << 'EOF'
import mathlist
main() {
    println(mathlist._internal_helper(5))
}
EOF
cd "$SCRIPT_DIR"
if AETHER_HOME="" "$ROOT/build/ae" build /tmp/ae_exports_list_bad1.ae \
        -o /tmp/ae_exports_list_bad1 2>/dev/null; then
    echo "  [FAIL] non-listed function should be rejected"
    fail=$((fail + 1))
else
    echo "  [PASS] non-listed function correctly rejected"
    pass=$((pass + 1))
fi

# Test 2: accessing a non-listed constant should fail.
cat > /tmp/ae_exports_list_bad2.ae << 'EOF'
import mathlist
main() {
    println(mathlist.SECRET)
}
EOF
if AETHER_HOME="" "$ROOT/build/ae" build /tmp/ae_exports_list_bad2.ae \
        -o /tmp/ae_exports_list_bad2 2>/dev/null; then
    echo "  [FAIL] non-listed const should be rejected"
    fail=$((fail + 1))
else
    echo "  [PASS] non-listed const correctly rejected"
    pass=$((pass + 1))
fi

rm -f /tmp/ae_exports_list_bad*.ae /tmp/ae_exports_list_bad*

if [ $fail -gt 0 ]; then
    echo "FAIL: $fail rejection(s) didn't fire"
    exit 1
fi
echo "exports-list rejection tests: $pass passed"
