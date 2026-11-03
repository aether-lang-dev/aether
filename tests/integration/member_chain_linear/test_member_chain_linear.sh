#!/bin/sh
# Regression (#3021): type checking a chain of field reads costs memory
# linear in its length.
#
# infer_type's member-access case inferred its object once per check it
# made (`.len`, lanes, bitstructs, the field lookup), and each of those
# re-inferred the whole chain under it; the field lookup also leaked the
# object's type on every path that found the field. `n.next.next ... .v`
# took about 4x more memory per link: 127 MB at 12 links, 1.7 GB at 14. A
# 150-link test chain grew one aetherc to 22 GB and took a 32 GB machine
# down with it. The object is now inferred once per access and freed once.
#
# Every compiler call here, `ae run` included, runs under a 1 GB
# address-space limit where the shell can set one, so a regression fails
# this test instead of exhausting the machine; the limit reaches the
# aetherc that `ae` starts (checked on Windows/MSYS). Where it cannot be
# set, the chains stop at 14 links, which the old code also survived.
# Nothing is run after a check that failed. An uncapped `ae run` of the
# 30-link ring below, against the old compiler, is what hard-reset a 32 GB
# machine while this test was being written.
#
# The programs are generated here by shell loops.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] member_chain_linear: ae not built"; exit 0; }

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP" || :; return 0; }
trap cleanup EXIT

fail=0
LIMIT_KB=1048576
# An AddressSanitizer build reserves terabytes of address space for its
# shadow memory and cannot start under an address-space limit (#3539), so
# under ASan the test takes its no-limit path.
if [ "${AETHER_SANITIZER:-}" != asan ] && (ulimit -v "$LIMIT_KB") 2>/dev/null; then
    LINKS=99    # the parser's postfix limit is 100 operations
    RING=30
else
    LINKS=14
    RING=14
    echo "  [NOTE] no address-space limit here: chains of $LINKS links"
fi

# capped CMD...: CMD under the address-space limit, where it can be set.
capped() {
    if [ "${AETHER_SANITIZER:-}" = asan ]; then "$@"; return; fi
    (ulimit -v "$LIMIT_KB" 2>/dev/null; "$@")
}

# rep TEXT N: TEXT, N times.
rep() {
    i=0
    while [ "$i" -lt "$2" ]; do printf '%s' "$1"; i=$((i + 1)); done
}

# check_ok NAME: `ae check` passes $TMP/NAME.ae within the limit; returns
# non-zero when it does not.
check_ok() {
    if capped "$AE" check "$TMP/$1.ae" >"$TMP/$1.log" 2>&1; then
        echo "  [PASS] $1"
        return 0
    fi
    echo "  [FAIL] $1: ae check failed, or ran out of memory"
    sed 's/^/        /' "$TMP/$1.log" | head -8
    fail=1
    return 1
}

# A chain through `*Node`, the pointer-to-struct field lookup.
{
    printf 'struct Node { next: *Node; v: int }\n\nmain() {\n'
    printf '    *Node n = null\n    if n != null {\n        x = n'
    rep '.next' "$LINKS"
    printf '.v\n        println(x)\n    }\n    println("ok")\n}\n'
} > "$TMP/pointer.ae"
check_ok pointer

# The same chain read from a parameter, in a returned expression.
{
    printf 'struct Node { next: *Node; v: int }\n\nlast(n: *Node) -> int {\n    return n'
    rep '.next' "$LINKS"
    printf '.v\n}\n\nmain() {\n    println("ok")\n}\n'
} > "$TMP/param.ae"
check_ok param

# A chain through nested struct values, a different type at every link: the
# value-struct field lookup.
{
    printf 'struct S%d { v: int }\n' "$LINKS"
    i=$((LINKS - 1))
    while [ "$i" -ge 0 ]; do
        printf 'struct S%d { s: S%d }\n' "$i" $((i + 1))
        i=$((i - 1))
    done
    printf '\ndepth(x: S0) -> int {\n    return x'
    rep '.s' "$LINKS"
    printf '.v\n}\n\nmain() {\n    println("ok")\n}\n'
} > "$TMP/values.ae"
check_ok values

# A chain inside an interpolation, beside a second one in arithmetic.
{
    printf 'struct Node { next: *Node; v: int }\n\nshow(n: *Node) -> string {\n'
    printf '    total = n'
    rep '.next' "$LINKS"
    printf '.v + n'
    rep '.next' "$LINKS"
    printf '.v\n    return "${n'
    rep '.next' "$LINKS"
    printf '.v} ${total}"\n}\n\nmain() {\n    println("ok")\n}\n'
} > "$TMP/interp.ae"
check_ok interp

# Built and run: a node whose `next` is itself, read $RING links deep, so
# the generated C follows the chain too. Only after its check passed.
{
    printf 'struct Node { next: *Node; v: int }\n\nmain() {\n'
    printf '    a = Node { next: null, v: 7 }\n    *Node p = &a\n    a.next = p\n'
    printf '    println(p'
    rep '.next' "$RING"
    printf '.v)\n}\n'
} > "$TMP/ring.ae"
if check_ok ring; then
    out="$(capped "$AE" run "$TMP/ring.ae" 2>&1)"
    if [ "$out" = "7" ]; then
        echo "  [PASS] ring runs"
    else
        echo "  [FAIL] ring: expected 7, got:"
        printf '%s\n' "$out" | sed 's/^/        /' | tail -8
        fail=1
    fi
fi

exit $fail
