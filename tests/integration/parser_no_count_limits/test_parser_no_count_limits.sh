#!/bin/sh
# Regression (#2907): the parser counts nothing it does not need to, and the
# limits it keeps are reported.
#
# parse_program stopped after 10000 top-level declarations at a "safety
# limit" whose message (parser_message) printed nothing, so aetherc exited 1
# without a word. The two `when` region loops had the same cap; a switch
# stopped at 1000 cases, a match at 1000 arms, a case block and an actor
# body at 1000 statements, a postfix chain at 100 operations, all silently,
# with the rest read as something else. Every loop now guarantees forward
# progress instead of counting, and every parser message is a diagnostic.
#
# What the binary-operator cap was protecting, without saying so, is the
# stack: a chain of operators nests the tree as deep as it is long and
# every later pass walks it recursively. On Windows aetherc overflowed at
# about 600 chained `+ 1`. A chain over 1000 is now E0100 naming the limit,
# and a chain of 1000 builds and runs: the compiler runs on a stack of its
# own on every platform (compiler/aether_compile_stack.h, #3539), so it
# does under AddressSanitizer too.
#
# The programs are generated here by shell loops.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] parser_no_count_limits: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0

# check_ok NAME: `ae check` passes $TMP/NAME.ae.
check_ok() {
    if "$AE" check "$TMP/$1.ae" >"$TMP/$1.log" 2>&1; then
        echo "  [PASS] $1"
    else
        echo "  [FAIL] $1: refused"
        sed 's/^/        /' "$TMP/$1.log" | head -8
        fail=1
    fi
}

# run_ok NAME WANT: `ae run` of $TMP/NAME.ae prints WANT.
run_ok() {
    out="$("$AE" run "$TMP/$1.ae" 2>&1)"
    if [ "$out" = "$2" ]; then
        echo "  [PASS] $1"
    else
        echo "  [FAIL] $1: expected '$2', got:"
        printf '%s\n' "$out" | sed 's/^/        /' | tail -8
        fail=1
    fi
}

# 10500 clauses of one function, a catch-all and main: 10502 declarations.
i=0
while [ $i -lt 10500 ]; do echo "f($i) -> $((2 * i))"; i=$((i + 1)); done > "$TMP/clauses.ae"
printf 'f(_) -> -1\n\nmain() {\n    println(f(10499))\n    println(f(20000))\n}\n' >> "$TMP/clauses.ae"
run_ok clauses "$(printf '20998\n-1')"

# 10100 declarations inside a `when` region that is taken.
{
    echo 'when !defined(AE_TEST_NEVER_DEFINED) {'
    i=0
    while [ $i -lt 10100 ]; do echo "    const C$i = $i"; i=$((i + 1)); done
    echo '}'
    printf 'main() {\n    println(C10099)\n}\n'
} > "$TMP/region.ae"
check_ok region

# A switch of 1200 cases, the last one's block of 1200 statements.
{
    printf 'main() {\n    n = 1199\n    t = 0\n    switch n {\n'
    i=0
    while [ $i -lt 1199 ]; do printf '        case %d:\n            t = %d\n' $i $i; i=$((i + 1)); done
    printf '        case 1199:\n'
    i=0
    while [ $i -lt 1200 ]; do printf '            t = t + 1\n'; i=$((i + 1)); done
    printf '        default:\n            t = -1\n    }\n    println(t)\n}\n'
} > "$TMP/switch.ae"
check_ok switch

# A match of 1200 arms.
{
    printf 'main() {\n    n = 1199\n    v = match n {\n'
    i=0
    while [ $i -lt 1200 ]; do printf '        %d -> %d\n' $i $((i * 3)); i=$((i + 1)); done
    printf '        _ -> -1\n    }\n    println(v)\n}\n'
} > "$TMP/match.ae"
check_ok match

# An actor of 1100 state fields.
{
    printf 'message Get {}\n\nactor Big {\n'
    i=0
    while [ $i -lt 1100 ]; do printf '    state s%d = %d\n' $i $i; i=$((i + 1)); done
    printf '    receive {\n        Get() -> {\n            println(s1099)\n        }\n    }\n}\n\n'
    printf 'main() {\n    b = spawn(Big())\n    b ! Get {}\n}\n'
} > "$TMP/actor.ae"
check_ok actor

# A postfix chain of 150 field reads (the cap was 100).
{
    printf 'struct Node { next: *Node; v: int }\n\nmain() {\n    *Node n = null\n    if n != null {\n        x = n'
    i=0
    while [ $i -lt 150 ]; do printf '.next'; i=$((i + 1)); done
    printf '.v\n        println(x)\n    }\n    println("ok")\n}\n'
} > "$TMP/postfix.ae"
check_ok postfix

# 1000 chained operators build and run; 1001 are refused, naming the limit,
# at the operator past it.
sum() {
    printf 'main() {\n    x = 1'
    i=0
    while [ $i -lt "$1" ]; do printf ' + 1'; i=$((i + 1)); done
    printf '\n    println(x)\n}\n'
}
sum 1000 > "$TMP/chain1000.ae"
run_ok chain1000 1001
sum 1001 > "$TMP/chain1001.ae"
out="$("$AE" check "$TMP/chain1001.ae" 2>&1)"
first=$(printf '%s\n' "$out" | grep -a -m1 '^error')
at=$(printf '%s\n' "$out" | grep -a -m1 -- '-->' | sed 's/.*\.ae://')
case "$first" in
    "error[E0100]: this expression chains more than 1000 operations, the limit")
        if [ "$at" = "2:4011" ]; then
            echo "  [PASS] chain1001"
        else
            echo "  [FAIL] chain1001: reported at $at, expected 2:4011"
            fail=1
        fi
        ;;
    *)
        echo "  [FAIL] chain1001: expected the chain limit error, got: $first"
        fail=1
        ;;
esac

exit $fail
