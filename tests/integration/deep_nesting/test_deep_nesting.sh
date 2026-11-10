#!/bin/sh
# #3574: the C of an expression nests by a constant, however deep the tree
# the parser takes, and however long the chain it builds a link a token.
# Each shape below nested its C one to four brackets a level, and Clang
# refuses C nested more than 256 deep:
#
#   casts:    `x as long as int ...`, 1000 casts (a postfix chain)
#   ufcs:     `x.inc().inc()...`, 500 calls (as many as the parser takes)
#   nots:     `!!!...x`, 508 (as deep as the parser takes)
#   calls:    `g(f(0), g(f(1), ...))`, 253 deep (as deep as the parser takes)
#   interps:  `"${x} ${"${x} ${"..."}"}"`, 999 deep
#   ifexpr:   an `if` expression with an `else if` chain of 504 links
#   elseif:   an `if` statement with an `else if` chain of 506 links
#
# Every 24 levels of an expression a node is held in a temporary ahead of
# its statement, in the order the statement evaluates it (calls and
# conditions here record theirs); `else if` chains of both kinds past 16
# links are written flat, each condition run only when no branch before it
# was taken. Each program is checked for what it prints and for the depth
# of its C after the sequence macros, as a GCC-compatible compiler expands
# them.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
NAME=deep_nesting

if [ ! -x "$AE" ]; then
    echo "  [SKIP] $NAME: $AE not built"
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fails=0
depths=""

# The deepest ( [ { of a C file outside literals, comments and directives,
# with the sequence macros spelled as a GCC-compatible compiler has them.
c_depth() {
    LC_ALL=C awk 'BEGIN { d = 0; m = 0; com = 0 }
         /^[ \t]*#/ { next }
         { gsub(/_AE_SEQ_OPEN/, "({"); gsub(/_AE_SEQ_CLOSE/, "; })")
           n = length($0); str = 0
           for (i = 1; i <= n; i++) {
               c = substr($0, i, 1)
               if (com) { if (c == "*" && substr($0, i + 1, 1) == "/") { com = 0; i++ } continue }
               if (str) { if (c == "\\") { i++; continue } if (c == q) str = 0; continue }
               if (c == "/" && substr($0, i + 1, 1) == "*") { com = 1; i++; continue }
               if (c == "/" && substr($0, i + 1, 1) == "/") break
               if (c == "\"" || c == "\047") { str = 1; q = c; continue }
               if (c == "(" || c == "[" || c == "{") { d++; if (d > m) m = d }
               else if (c == ")" || c == "]" || c == "}") d--
           } }
         END { print m }' "$1"
}

# shape <name> <want>: $WORK/<name>.ae prints <want> last, and its C nests
# no deeper than 64.
shape() {
    got="$(cd "$WORK" && AETHER_HOME="$ROOT" "$AE" run "$1.ae" 2>&1 | tail -1 | tr -d '\r')"
    if [ "$got" != "$2" ]; then
        echo "  [FAIL] $NAME: $1: not \"$2\":"
        printf '%s\n' "$got" | cut -c1-160 | sed 's/^/        /'
        fails=$((fails + 1))
    fi
    AETHER_HOME="$ROOT" "$AETHERC" "$WORK/$1.ae" "$WORK/$1.c" >/dev/null 2>&1
    d="$(c_depth "$WORK/$1.c")"
    depths="$depths $1=$d"
    if [ -z "$d" ] || [ "$d" -gt 64 ]; then
        echo "  [FAIL] $NAME: $1: its C nests ${d:-?} brackets deep"
        fails=$((fails + 1))
    fi
}

ORDER='var g_last = -1
var g_bad = 0
f(i: int) -> int {
    if i != g_last + 1 { g_bad = g_bad + 1 }
    g_last = i
    return i
}'

awk 'BEGIN { print "main() {\n    x = 3"; printf "    v = x"
             for (i = 0; i < 500; i++) printf " as long as int"
             print "\n    println(\"casts ${v}\")\n}" }' > "$WORK/casts.ae"
shape casts "casts 3"

awk 'BEGIN { print "inc(x: int) -> int { return x + 1 }\nmain() {\n    x = 3"; printf "    v = x"
             for (i = 0; i < 500; i++) printf ".inc()"
             print "\n    println(\"ufcs ${v}\")\n}" }' > "$WORK/ufcs.ae"
shape ufcs "ufcs 503"

awk 'BEGIN { print "main() {\n    x = true"; printf "    v = "
             for (i = 0; i < 508; i++) printf "!"
             print "x\n    println(\"nots ${v}\")\n}" }' > "$WORK/nots.ae"
shape nots "nots true"

{ printf '%s\n' "$ORDER"; awk 'BEGIN {
    print "g(a: int, b: int) -> int { return a + b }\nmain() {"; printf "    v = "
    for (i = 0; i < 253; i++) printf "g(f(%d), ", i
    printf "0"
    for (i = 0; i < 253; i++) printf ")"
    print "\n    println(\"calls ${v} ${g_bad}\")\n}" }'; } > "$WORK/calls.ae"
shape calls "calls 31878 0"

awk 'BEGIN { print "import std.string\nmain() {\n    x = 5"; printf "    v = \"${x}"
             for (i = 1; i < 999; i++) printf " ${\"${x}"
             for (i = 1; i < 999; i++) printf "\"}"
             print "\"\n    println(\"interps ${string.length(v)}\")\n}" }' > "$WORK/interps.ae"
shape interps "interps 1997"

# The conditions record the order they run in: 0 to 7, none after the
# taken one (8 links ran, g_last 7).
{ printf '%s\n' "$ORDER"; awk 'BEGIN {
    print "main() {"
    print "    x = 7"
    printf "    v = if f(0) == x { 0 }"
    for (i = 1; i < 504; i++) printf " else if f(%d) == x { %d }", i, i * 2
    print " else { -1 }"
    print "    println(\"ifexpr ${v} ${g_last} ${g_bad}\")"
    print "}" }'; } > "$WORK/ifexpr.ae"
shape ifexpr "ifexpr 14 7 0"

# The same with statements, a variable every branch binds, and a loop
# whose branches `continue`, `break` and fall through.
{ printf '%s\n' "$ORDER"; awk 'BEGIN {
    print "pick(x: int) -> int {"
    printf "    if f(0) == x { v = 0 }"
    for (i = 1; i < 506; i++) printf " else if f(%d) == x { v = %d }", i, i * 3
    print " else { v = -1 }"
    print "    return v"
    print "}"
    print "main() {"
    print "    w = pick(7)"
    print "    println(\"elseif ${w} ${g_last} ${g_bad}\")"
    print "    n = 0"
    print "    i = 0"
    print "    while i < 40 {"
    print "        i = i + 1"
    printf "        if i == 1 { continue }"
    for (k = 2; k < 30; k++) printf " else if i == %d { n = n + %d }", k, k
    print " else if i == 30 { break } else { n = n + 1000 }"
    print "        n = n + 1"
    print "    }"
    print "    g_last = -1"
    print "    println(\"elseif ${pick(505)} ${g_last} ${g_bad} ${n} ${i}\")"
    print "}" }'; } > "$WORK/elseif.ae"
# n: 2 + ... + 29 = 434, plus 1 for each of i = 2 .. 29 (28); the loop ends at 30.
shape elseif "elseif 1515 505 0 462 30"

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] $NAME: casts, method chains, nots, calls, interpolations and else-if chains as deep as the parser takes nest the C by a constant (depth$depths)"
exit 0
