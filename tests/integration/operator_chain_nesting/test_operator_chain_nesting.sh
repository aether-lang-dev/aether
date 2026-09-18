#!/bin/sh
# Regression (#2071): a long left-associated operator chain lowers to flat
# C. Codegen used to parenthesise every binary node, so `x0 + x1 + … + xN`
# came out as `((((x0 + x1) + x2) + …)` with N nesting levels, and clang
# refuses more than 256 ("bracket nesting level exceeded maximum of 256").
# A link of the same precedence level on the LEFT needs no brackets of its
# own — C already groups `a - b - c` as `(a - b) - c` — so the emitted
# expression nests with the expression's real depth, not its length.
#
# Checks, on generated sources: a 400-term sum and a 300-term chain of
# mixed `-` links whose right operands must keep their brackets, both
# for the value they compute and for the bracket depth of the C line.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] operator_chain_nesting: $AE not built"
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fail=0

awk 'BEGIN {
    print "main() {"
    for (i = 0; i <= 400; i++) printf "    x%d = %d\n", i, i
    printf "    s = "
    for (i = 0; i <= 400; i++) printf "%sx%d", (i ? " + " : ""), i
    print ""
    printf "    p = "
    for (i = 0; i <= 300; i++) printf "%s(x%d * 2 - 1)", (i ? " - " : ""), i
    print ""
    print "    println(\"s=${s} p=${p}\")"
    print "}"
}' > "$WORK/chain.ae"

# s = 0 + 1 + … + 400; p = (0*2-1) - sum_{i=1..300}(2i - 1) = -1 - 300^2
want="s=80200 p=-90001"
got="$(AETHER_HOME="$ROOT" "$AE" run "$WORK/chain.ae" 2>&1 | tail -1)"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] operator_chain_nesting: expected '$want', got '$got'"
    fail=1
fi

max_depth() {
    awk '{ d = 0; m = 0
           for (i = 1; i <= length($0); i++) {
               c = substr($0, i, 1)
               if (c == "(") d++
               if (c == ")") d--
               if (d > m) m = d
           }
           print m }'
}
AETHER_HOME="$ROOT" "$AETHERC" "$WORK/chain.ae" "$WORK/chain.c" >/dev/null 2>&1
depth_s="$(grep 'x399 + x400' "$WORK/chain.c" | head -1 | max_depth)"
depth_p="$(grep 'x300 \* 2' "$WORK/chain.c" | head -1 | max_depth)"
if [ -z "$depth_s" ] || [ "$depth_s" -gt 4 ]; then
    echo "  [FAIL] operator_chain_nesting: the 400-term sum nests ${depth_s:-?} brackets deep"
    fail=1
fi
if [ -z "$depth_p" ] || [ "$depth_p" -gt 6 ]; then
    echo "  [FAIL] operator_chain_nesting: the mixed chain nests ${depth_p:-?} brackets deep"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] operator_chain_nesting: 400-term chains lower flat (depth $depth_s / $depth_p) and compute correctly"
fi
exit $fail
