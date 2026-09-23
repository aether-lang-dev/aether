#!/bin/sh
# Regression (#2175): a multi-value `return` whose slots after the first are
# PARAMETERS.
#
# Constraint collection visited only the first child of a return statement,
# so in `return lo, hi` only `lo` was ever typed. A local in a later slot was
# rescued by infer_return_type_impl reading its declaration back; a parameter
# has no declaration to read, so it stayed UNKNOWN. Codegen then reported
# "unresolved type in codegen, defaulting to int" at the function, its
# return and every destructuring caller -- 16 warnings in every program that
# imported std.cryptography.des3, all pointing into the standard library.
# And "defaulting to int" is not only noise: a string or long parameter in
# that slot was emitted as an int.
#
# This compiles programs with aetherc directly (ae run keeps codegen warnings
# to itself) and fails on the warning, then runs them to prove each slot
# carries its real type.
set -eu

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
if [ ! -x "$AE" ] || [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] tuple_return_params: build/ae or build/aetherc not built"
    exit 0
fi
# Resolve std from this tree, not an installed copy.
AETHER_HOME=""
export AETHER_HOME

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fail() {
    echo "  [FAIL] tuple_return_params: $1"
    exit 1
}

no_unresolved() {
    "$AETHERC" "$1" "$WORK/out.c" > "$WORK/cc.log" 2>&1 \
        || { sed 's/^/    /' "$WORK/cc.log" | head -20; fail "$2: aetherc rejected it"; }
    if grep -q 'unresolved type' "$WORK/cc.log"; then
        grep -A3 'unresolved type' "$WORK/cc.log" | sed 's/^/    /' | head -20
        fail "$2: codegen fell back to int for a tuple slot"
    fi
}

# Parameters in every slot, of three different types, so a slot that fell
# back to int shows up as a wrong value, not just a warning.
cat > "$WORK/params.ae" <<'AE'
extern exit(code: int)

swap(hi: int, lo: int) -> {
    return lo, hi
}

mixed(n: int, s: string, big: long) -> {
    return n, s, big
}

// a parameter after a local, and a local after a parameter
around(p: string) -> {
    x = 41
    return x + 1, p
}

main() {
    a, b = swap(1, 2)
    if a != 2 || b != 1 { println("FAIL swap: ${a} ${b}"); exit(1) }

    n, s, big = mixed(7, "seven", 7000000000)
    if n != 7 { println("FAIL n: ${n}"); exit(1) }
    if s != "seven" { println("FAIL s: ${s}"); exit(1) }
    if big != 7000000000 { println("FAIL big: ${big}"); exit(1) }

    v, p = around("kept")
    if v != 42 || p != "kept" { println("FAIL around: ${v} ${p}"); exit(1) }

    println("PASS")
}
AE
no_unresolved "$WORK/params.ae" "parameters in tuple slots"
out="$("$AE" run "$WORK/params.ae" 2>&1)" || { echo "$out" | sed 's/^/    /'; fail "parameters in tuple slots: the program failed"; }
[ "$(echo "$out" | tail -1)" = "PASS" ] || { echo "$out" | sed 's/^/    /'; fail "parameters in tuple slots: expected PASS"; }

# The same shape reached through an import, which is where it was found.
mkdir -p "$WORK/modcase/tup"
cat > "$WORK/modcase/tup/module.ae" <<'AE'
exports(run)

pair(hi: int, lo: int) -> {
    return lo, hi
}

run(x: int) -> int {
    hi, lo = pair(x, x + 1)
    return hi * 10 + lo
}
AE
cat > "$WORK/modcase/main.ae" <<'AE'
import tup

main() {
    println("${tup.run(3)}")
}
AE
( cd "$WORK/modcase" && no_unresolved main.ae "parameters in tuple slots, in a module" )
out="$(cd "$WORK/modcase" && "$AE" run main.ae 2>&1 | tail -1)"
[ "$out" = "43" ] || fail "module case: expected 43, got '$out'"

# And the standard-library function it was found in.
no_unresolved "$ROOT/tests/integration/crypto_block_ciphers/test_crypto_block_ciphers.ae" \
    "a program importing std.cryptography.des3"

echo "  [PASS] tuple_return_params: parameters type their tuple slots, with no int fallback"
