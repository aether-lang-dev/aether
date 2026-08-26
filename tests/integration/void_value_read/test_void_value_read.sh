#!/bin/sh
# Reading the value of a call that returns none is now a diagnostic.
#
# A function with no declared return type and no `return <value>` lowers to C
# `void` (deliberate, #354). Assigning from such a call used to compile
# silently and read the ABI return register -- stack residue, a different
# number every run. A build orchestrator read exactly that as a node's exit
# status, so a failing node could exit falsely GREEN.
#
# This asserts three things, and the third is the one that keeps the check
# honest: "call for effect" is the overwhelmingly common shape in the tree and
# must keep working untouched.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] void_value_read: ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

# ---- 1. the bug: assigning from a value-less call is rejected -------------
cat > "$TMP/bad.ae" <<'AEOF'
voidfn(x: int) { }
main() {
    v = voidfn(1)
    println("v=${v}")
    return 0
}
AEOF
if "$AE" run "$TMP/bad.ae" >"$TMP/bad.log" 2>&1; then
    fail "assigning from a value-less call was accepted (would read register garbage)"
fi
grep -q "returns no value" "$TMP/bad.log" || {
    echo "  [FAIL] rejected, but not with the value-less-call diagnostic:"
    sed -n '1,5p' "$TMP/bad.log" | sed 's/^/         /'
    exit 1
}
# The message must name the offending function and the way out, or it is no
# better than the C compiler's version of this error.
grep -q "voidfn" "$TMP/bad.log" || fail "diagnostic does not name the function"
grep -q "return type" "$TMP/bad.log" || fail "diagnostic does not say how to fix it"

# ---- 2. an annotated function is unaffected ------------------------------
cat > "$TMP/typed.ae" <<'AEOF'
intfn(x: int): int { return x * 2 }
main() {
    v = intfn(21)
    println("v=${v}")
    return 0
}
AEOF
OUT=$("$AE" run "$TMP/typed.ae" 2>&1) || fail "typed function rejected: $OUT"
[ "$OUT" = "v=42" ] || fail "typed function gave [$OUT], want v=42"

# ---- 3. call-for-effect still works, which is most of the tree -----------
# An unannotated function called as a STATEMENT is the ordinary case and must
# not be touched. If this ever fails the check has become far too eager.
cat > "$TMP/effect.ae" <<'AEOF'
shout(msg: string) { println(msg) }
main() {
    shout("hello")
    shout("again")
    return 0
}
AEOF
OUT=$("$AE" run "$TMP/effect.ae" 2>&1) || fail "call-for-effect rejected: $OUT"
[ "$OUT" = "hello
again" ] || fail "call-for-effect gave [$OUT]"

echo "  [PASS] void_value_read: value-less call assignment diagnosed, typed and effect-only calls unaffected"
