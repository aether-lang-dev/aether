#!/bin/bash
# Test that generated C compiles under strict C11 without GCC extensions.
# Forces AETHER_GCC_COMPAT=0 to exercise the MSVC fallback paths.
set -e

cd "$(dirname "$0")/../.."

echo "=== MSVC Compat Test: Verifying generated C compiles without GCC extensions ==="

# Helper: force AETHER_GCC_COMPAT=0 at the very top of a generated C file,
# before any #ifndef guard can auto-detect the compiler.
force_no_gcc_compat() {
    local f="$1"
    sed -i.bak '1s/^/#define AETHER_GCC_COMPAT 0\n/' "$f"
}

# Test 1: Non-actor program with string interpolation
cat > /tmp/test_msvc_interp.ae <<'AEOF'
get_msg(n) -> {
    msg = "count=${n}"
    msg
}

main() {
    r = get_msg(5)
    println(r)
}
AEOF

./build/aetherc /tmp/test_msvc_interp.ae /tmp/test_msvc_interp.c
force_no_gcc_compat /tmp/test_msvc_interp.c
# Compile with strict C11 — must succeed without GCC extensions
gcc -c -o /dev/null /tmp/test_msvc_interp.c \
    -I. -Iruntime -Iruntime/utils -Iruntime/actors -Istd/collections \
    -std=c11 -pedantic -Wno-unused-label -Werror 2>&1
echo "  [PASS] Non-actor string interpolation compiles under C11 -pedantic"

# Test 2: Actor program with computed goto fallback
# Use python3 to write the .ae file, avoiding shell escaping issues with '!'
python3 -c "
import sys
with open('/tmp/test_msvc_actor.ae', 'w') as f:
    f.write('message Ping { value: int }\n')
    f.write('\n')
    f.write('actor Pinger {\n')
    f.write('    state count = 0\n')
    f.write('\n')
    f.write('    receive {\n')
    f.write('        Ping(value) -> {\n')
    f.write('            count = count + value\n')
    f.write('        }\n')
    f.write('    }\n')
    f.write('}\n')
    f.write('\n')
    f.write('main() {\n')
    f.write('    p = spawn(Pinger())\n')
    f.write('    p ! Ping { value: 1 }\n')
    f.write('    println(\"ok\")\n')
    f.write('}\n')
"

./build/aetherc /tmp/test_msvc_actor.ae /tmp/test_msvc_actor.c
force_no_gcc_compat /tmp/test_msvc_actor.c

# Check that the switch-case fallback exists alongside computed goto
if grep -q 'goto \*dispatch_table' /tmp/test_msvc_actor.c; then
    if ! grep -q 'switch.*_msg_id' /tmp/test_msvc_actor.c; then
        echo "  [FAIL] Actor dispatch: missing switch fallback"
        exit 1
    fi
fi
echo "  [PASS] Actor dispatch has switch-case fallback"

# Compile actor (syntax-check only — can't link without full runtime)
gcc -fsyntax-only /tmp/test_msvc_actor.c \
    -I. -Iruntime -Iruntime/utils -Iruntime/actors -Iruntime/config \
    -Iruntime/scheduler -Istd/collections \
    -std=c11 -Wno-unused-label 2>&1
echo "  [PASS] Actor program syntax-checks under C11"

echo ""
echo "=== All MSVC compat tests passed ==="
