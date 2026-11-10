#!/bin/sh
# #3574: a file-scope constant's initializer is a C constant expression,
# which takes no temporary, so the C of a long chain in one nested a level
# a link: 1000 comparisons were 1000 brackets of C, 1000 shifts 3999, and
# Clang refuses C nested past 256. Past 32 levels the compiler now computes
# the constant, as the C would, and writes its value.
#
# Each constant below is a chain as long as the parser takes (1000 links,
# 400 `!`), and main computes the same expression at run time, where it is
# no constant: the two must agree (comparisons of mixed signedness, shifts
# masked and into the sign bit, the most negative int over -1, remainders,
# floats, a constant named in the chain, `!`). The C must nest no deeper
# than 64.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
NAME=deep_constants

if [ ! -x "$AE" ]; then
    echo "  [SKIP] $NAME: $AE not built"
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fails=0

awk 'function chain(first, op, n, vals,   s, i, nv, v) {
         nv = split(vals, v, ",")
         s = first
         for (i = 1; i < n; i++) s = s " " op " " v[(i % nv) + 1]
         return s
     }
     BEGIN {
         e[1] = chain("1", "<", 1000, "1,2,0")
         e[2] = chain("7", "<<", 1000, "0,1,40,0")
         e[3] = chain("7000000000", "<<", 1000, "0,1,70,0")
         e[4] = chain("1000000007", ">>", 1000, "0,1,0,33")
         e[5] = chain("(-2147483647 - 1)", "/", 1000, "1,-1,1")
         e[6] = chain("1000003", "%", 1000, "1000,97,13")
         e[7] = chain("18446744073709551615", ">", 1000, "-1,0,2")
         e[8] = chain("1.5", "<", 1000, "2.5,0.5,1.0")
         e[9] = chain("K", "<<", 1000, "1,0,2,0")
         s = "true"; for (i = 0; i < 400; i++) s = "!" s; e[10] = s
         e[11] = chain("1", "==", 1000, "1,0,1")
         n = 11
         print "const K = 3"
         for (i = 1; i <= n; i++) printf "const C%d = %s\n", i, e[i]
         print "main() {"
         print "    bad = 0"
         for (i = 1; i <= n; i++) {
             printf "    r%d = %s\n", i, e[i]
             printf "    if C%d != r%d {\n        bad = bad + 1\n", i, i
             printf "        println(\"C%d is ${C%d}, computed ${r%d}\")\n    }\n", i, i, i
         }
         print "    println(\"bad ${bad}\")"
         print "}"
     }' > "$WORK/consts.ae"

got="$(cd "$WORK" && AETHER_HOME="$ROOT" "$AE" run consts.ae 2>&1 | tail -3 | tr -d '\r')"
case "$got" in
    *"bad 0") ;;
    *)
        echo "  [FAIL] $NAME: the constants and the same expressions at run time disagree:"
        printf '%s\n' "$got" | cut -c1-200 | sed 's/^/        /'
        fails=$((fails + 1))
        ;;
esac

AETHER_HOME="$ROOT" "$AETHERC" "$WORK/consts.ae" "$WORK/consts.c" >/dev/null 2>&1
d="$(awk 'BEGIN { d = 0; m = 0 }
          /^[ \t]*#/ { next }
          { gsub(/_AE_SEQ_OPEN/, "({"); gsub(/_AE_SEQ_CLOSE/, "; })")
            # One split, not a substr($0, i, 1) per character: the awk of OpenBSD
            # rescans the line from its start on every substr, so a long line
            # (these hold 1000-link chains) took six minutes to scan there.
            n = split($0, ch, ""); str = 0
            for (i = 1; i <= n; i++) {
                c = ch[i]
                if (str) { if (c == "\\") { i++; continue } if (c == q) str = 0; continue }
                if (c == "/" && ch[i + 1] == "/") break
                if (c == "\"" || c == "\047") { str = 1; q = c; continue }
                if (c == "(" || c == "[" || c == "{") { d++; if (d > m) m = d }
                else if (c == ")" || c == "]" || c == "}") d--
            } }
          END { print m }' "$WORK/consts.c")"
if [ -z "$d" ] || [ "$d" -gt 64 ]; then
    echo "  [FAIL] $NAME: the C nests ${d:-?} brackets deep"
    fails=$((fails + 1))
fi

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] $NAME: 1000-link constant chains are written as the values the same expressions compute (the C nests $d)"
exit 0
