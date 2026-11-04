#!/bin/sh
# #3574: the C of a program nests its blocks as deep as the source does, and
# the parser takes blocks 512 levels deep, where Clang refuses C nested past
# 256 brackets unless told otherwise. `ae` gives a Clang driver
# -fbracket-depth sized from the parser's own limit (AETHER_C_BRACKET_DEPTH),
# so a program nested as deep as the parser takes builds and runs with
# whatever C compiler `ae` drives: Clang on macOS and FreeBSD (or wherever
# $AE_CC or $CC names it), GCC elsewhere, which sets no such limit.
#
# Three programs, each as deep as the parser takes: 508 blocks, 254 nested
# `for` loops (three brackets a level) and 508 nested `match` arms (two).
# Each must print what it computes, and the C of the deepest must nest past
# 256, or the test would not reach the limit it is about.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
NAME=block_nesting_cc

if [ ! -x "$AE" ]; then
    echo "  [SKIP] $NAME: $AE not built"
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fails=0

# runs <name> <want>: $WORK/<name>.ae prints <want> last.
runs() {
    got="$(cd "$WORK" && AETHER_HOME="$ROOT" "$AE" run "$1.ae" 2>&1 | tail -1 | tr -d '\r')"
    if [ "$got" != "$2" ]; then
        echo "  [FAIL] $NAME: $1: not \"$2\":"
        printf '%s\n' "$got" | cut -c1-200 | sed 's/^/        /'
        fails=$((fails + 1))
    fi
}

awk 'BEGIN { print "main() {\n    x = 0"; for (i = 0; i < 508; i++) print "{"
             print "x = x + 1"; for (i = 0; i < 508; i++) print "}"
             print "    println(\"blocks ${x}\")\n}" }' > "$WORK/blocks.ae"
runs blocks "blocks 1"

# Each loop runs once: the body, 254 loops deep, runs once.
awk 'BEGIN { print "main() {\n    x = 0"
             for (i = 0; i < 254; i++) printf "for j%d in 0..1 {\n", i
             print "x = x + 1"
             for (i = 0; i < 254; i++) print "}"
             print "    println(\"fors ${x}\")\n}" }' > "$WORK/fors.ae"
runs fors "fors 1"

awk 'BEGIN { print "main() {\n    x = 0"
             for (i = 0; i < 508; i++) print "match x {\n0 -> {"
             print "x = 0"
             for (i = 0; i < 508; i++) print "}\n_ -> {}\n}"
             print "    println(\"matches ${x}\")\n}" }' > "$WORK/matches.ae"
runs matches "matches 0"

# The deepest ( [ { of the C of the matches, outside literals and comments.
AETHER_HOME="$ROOT" "$AETHERC" "$WORK/matches.ae" "$WORK/matches.c" >/dev/null 2>&1
d="$(awk 'BEGIN { d = 0; m = 0 }
          /^[ \t]*#/ { next }
          { n = length($0); str = 0
            for (i = 1; i <= n; i++) {
                c = substr($0, i, 1)
                if (str) { if (c == "\\") { i++; continue } if (c == q) str = 0; continue }
                if (c == "/" && substr($0, i + 1, 1) == "/") break
                if (c == "\"" || c == "\047") { str = 1; q = c; continue }
                if (c == "(" || c == "[" || c == "{") { d++; if (d > m) m = d }
                else if (c == ")" || c == "]" || c == "}") d--
            } }
          END { print m }' "$WORK/matches.c")"
if [ -z "$d" ] || [ "$d" -le 256 ]; then
    echo "  [FAIL] $NAME: the C of 508 nested matches nests ${d:-?} deep, not past Clang's default 256"
    fails=$((fails + 1))
fi

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] $NAME: blocks, loops and matches as deep as the parser takes build and run (the deepest C nests $d)"
exit 0
