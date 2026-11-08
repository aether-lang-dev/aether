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
#
# #3574: a chain of calls, evaluated left to right. Each operator held its
# operands in a statement expression of its own (_AE_SEQ_OPEN), inside the
# one of the operator after it: three brackets a term, and Clang refused a
# sum of 86 calls. One sequence now holds every operand of the chain, and
# of calls inside calls. Checked on a 1000-term sum of calls, calls nested
# 200 deep on the left and a chain of calls inside calls, for the values,
# the order the calls ran in, and the depth of the C after the macros.

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

# The calls: f checks it runs in source order and counts the ones that do not.
awk 'BEGIN {
    print "var g_last = -1"
    print "var g_bad = 0"
    print "f(i: int) -> int {"
    print "    if i != g_last + 1 { g_bad = g_bad + 1 }"
    print "    g_last = i"
    print "    return i"
    print "}"
    print "g(a: int, b: int) -> int { return a + b }"
    print "main() {"
    printf "    c = f(0)"
    for (i = 1; i < 1000; i++) printf " + f(%d)", i
    print ""
    print "    println(\"c=${c} bad=${g_bad}\")"
    print "    g_last = -1"
    printf "    n = "
    for (i = 0; i < 200; i++) printf "g("
    printf "f(0)"
    for (i = 1; i <= 200; i++) printf ", f(%d))", i
    print ""
    print "    println(\"n=${n} bad=${g_bad}\")"
    print "    g_last = -1"
    printf "    m = g(f(0), f(1))"
    for (i = 1; i < 300; i++) printf " + g(f(%d), f(%d))", 2 * i, 2 * i + 1
    print ""
    print "    println(\"m=${m} bad=${g_bad}\")"
    print "}"
}' > "$WORK/calls.ae"
# c = 0 + 1 + ... + 999; n = 0 + 1 + ... + 200; m = 0 + 1 + ... + 599.
want="c=499500 bad=0
n=20100 bad=0
m=179700 bad=0"
got="$(AETHER_HOME="$ROOT" "$AE" run "$WORK/calls.ae" 2>&1 | tail -3 | tr -d '\r')"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] operator_chain_nesting: the chains of calls printed:"
    printf '%s\n' "$got" | sed 's/^/        /'
    fail=1
fi

# The deepest ( [ { of a line, with the sequence macros spelled as a
# GCC-compatible compiler (Clang among them) expands them.
# Linear in the line: keep only the brackets, then walk them as an array.
# A substr() a character on the whole line is quadratic in FreeBSD's awk,
# whose substr and length count UTF-8 characters from the start each call:
# the 1000-link lines took this test past the sweep's 420 s limit there.
max_seq_depth() {
    LC_ALL=C awk '{ gsub(/_AE_SEQ_OPEN/, "({"); gsub(/_AE_SEQ_CLOSE/, "; })")
           gsub(/[^][(){}]/, "")
           n = split($0, ch, "")
           d = 0; m = 0
           for (i = 1; i <= n; i++) {
               c = ch[i]
               if (c == "(" || c == "[" || c == "{") d++
               else d--
               if (d > m) m = d
           }
           print m }'
}
AETHER_HOME="$ROOT" "$AETHERC" "$WORK/calls.ae" "$WORK/calls.c" >/dev/null 2>&1
depth_c="$(grep 'int c = _AE_SEQ_OPEN' "$WORK/calls.c" | head -1 | max_seq_depth)"
depth_n="$(grep 'int n = _AE_SEQ_OPEN' "$WORK/calls.c" | head -1 | max_seq_depth)"
depth_m="$(grep 'int m = _AE_SEQ_OPEN' "$WORK/calls.c" | head -1 | max_seq_depth)"
for d in "c:$depth_c" "n:$depth_n" "m:$depth_m"; do
    v="${d#*:}"
    if [ -z "$v" ] || [ "$v" -gt 12 ]; then
        echo "  [FAIL] operator_chain_nesting: the C of chain ${d%%:*} nests ${v:-?} brackets deep"
        fail=1
    fi
done

# #3574: the operators whose links nested C once more each, 1000 links of
# each: `&&` and `||` (a pair of brackets a link), comparisons (C reads
# `a < b < c` the same unbracketed but warns), integer `/` and `%` (a
# checked call a link) and `<<` and `>>` (a masked cast a link). `&&` and
# `||` are links of #2071's rule now; the others are written link by link,
# left to right, a division by zero raised where its link is, before the
# operands after it are evaluated (bump() must not run), MIN / -1 giving
# MIN. awk computes what each chain gives.
awk 'BEGIN {
    N = 1000
    print "var g_last = -1"
    print "var g_bad = 0"
    print "var g_after = 0"
    print "t(i: int, at: int) -> bool {"
    print "    if i != g_last + 1 { g_bad = g_bad + 1 }"
    print "    g_last = i"
    print "    return i != at"
    print "}"
    print "bump() -> int {"
    print "    g_after = g_after + 1"
    print "    return 1"
    print "}"
    print "main() {"
    print "    let a: int[5] = [1000000, 1, 2, 0, 33]"
    print "    let c: int[3] = [1000003, 7, 5]"
    print "    let b: bool[2] = [true, false]"
    print "    let m: int[3] = [-2147483647 - 1, -1, 1]"
    printf "    and_v = t(0, 500)"
    for (i = 1; i < N; i++) printf " && t(%d, 500)", i
    print ""
    print "    println(\"and=${and_v} last=${g_last} bad=${g_bad}\")"
    print "    g_last = -1"
    printf "    or_v = !t(0, 500)"
    for (i = 1; i < N; i++) printf " || !t(%d, 500)", i
    print ""
    print "    println(\"or=${or_v} last=${g_last} bad=${g_bad}\")"
    lt = 1000000; eq = 1; dv = 1000000; md = 1000003; sl = 1000000; sr = 1000000
    printf "    lt_v = a[0]"
    for (i = 1; i < N; i++) { r = i % 2 ? 1 : 2; printf " < a[%d]", r; lt = lt < r ? 1 : 0 }
    print ""
    printf "    eq_v = b[0]"
    for (i = 1; i < N; i++) { x = i % 2 ? 0 : 1; printf " == b[%d]", 1 - x; eq = eq == x ? 1 : 0 }
    print ""
    printf "    div_v = a[0]"
    for (i = 1; i < N; i++) { d = i % 2 ? 1 : 2; printf " / a[%d]", d; dv = int(dv / d) }
    print ""
    printf "    mod_v = c[0]"
    for (i = 1; i < N; i++) { k = i % 2 ? 1 : 2; printf " %% c[%d]", k; md = md % (k == 1 ? 7 : 5) }
    print ""
    printf "    shl_v = a[0]"
    for (i = 1; i < N - 1; i++) printf " << a[3]"
    print " << a[4]"
    sl = sl * 2
    printf "    shr_v = a[0]"
    for (i = 1; i < N - 1; i++) printf " >> a[3]"
    print " >> a[4]"
    sr = int(sr / 2)
    printf "    min_v = m[0] / m[1]"
    for (i = 2; i < N; i++) printf " / m[2]"
    print ""
    print "    println(\"lt=${lt_v} eq=${eq_v} div=${div_v} mod=${mod_v} shl=${shl_v} shr=${shr_v} min=${min_v}\")"
    print "    why = \"\""
    print "    try {"
    printf "        z_v = a[0]"
    for (i = 1; i < N - 1; i++) printf " / a[%d]", (i == 500 ? 3 : 1)
    print " / bump()"
    print "        println(\"z=${z_v}\")"
    print "    } catch e {"
    print "        why = e"
    print "    }"
    print "    println(\"caught=${why != \"\"} after=${g_after}\")"
    print "}"
    printf "and=false last=500 bad=0\nor=true last=500 bad=0\n" > "/dev/stderr"
    printf "lt=%s eq=%s div=%d mod=%d shl=%d shr=%d min=-2147483648\n", (lt ? "true" : "false"), (eq ? "true" : "false"), dv, md, sl, sr > "/dev/stderr"
    printf "caught=true after=0\n" > "/dev/stderr"
}' > "$WORK/ops.ae" 2> "$WORK/ops.want"
got="$(AETHER_HOME="$ROOT" "$AE" run "$WORK/ops.ae" 2>&1 | tail -4 | tr -d '\r')"
want="$(cat "$WORK/ops.want")"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] operator_chain_nesting: the operator chains printed:"
    printf '%s\n' "$got" | sed 's/^/        /'
    echo "      not:"
    printf '%s\n' "$want" | sed 's/^/        /'
    fail=1
fi
AETHER_HOME="$ROOT" "$AETHERC" "$WORK/ops.ae" "$WORK/ops.c" >/dev/null 2>&1
depths=""
for v in and_v or_v lt_v eq_v div_v mod_v shl_v shr_v min_v z_v; do
    d="$(grep "$v = " "$WORK/ops.c" | head -1 | max_seq_depth)"
    depths="$depths $d"
    if [ -z "$d" ] || [ "$d" -gt 12 ]; then
        echo "  [FAIL] operator_chain_nesting: the C of the 1000-link $v chain nests ${d:-?} brackets deep"
        fail=1
    fi
done

if [ "$fail" = 0 ]; then
    echo "  [PASS] operator_chain_nesting: 400-term chains lower flat (depth $depth_s / $depth_p) and compute correctly; chains of calls in order, as one sequence (depth $depth_c / $depth_n / $depth_m); 1000-link && || < == / % << >> chains in order (depth$depths)"
fi
exit $fail
