#!/bin/sh
# Regression (#2059): the size of a source file is bounded by memory, not by
# a token cap. The compiler used to lex the main file into a 50,000-entry
# stack array and refuse anything larger ("split into multiple files using
# imports" — no help to a generated single-entry program), lex an imported
# module into a 100,000-entry heap array and silently truncate the rest, and
# lex a `${...}` expression into 512 entries and silently drop its tail.
# Every one of those now goes through lexer_tokenize, which grows with the
# input, so this drives each former cap past its old limit:
#
#   main file      6,000 functions  (~72k tokens, old cap 50,000)
#   imported module 9,000 functions (~126k tokens, old cap 100,000)
#   interpolation   ${x0 * 1 + ... + x149 * 1} (600 tokens, old cap 512)
#
# The interpolation is spelled with a `* 1` per term rather than as a
# longer plain sum because the generated C parenthesises every binary
# node, and clang refuses more than 256 nested brackets (#2071); 150
# terms stay under that while still passing the old token cap.
#
# The sources are generated into a temp dir so nothing this large is
# committed. The program calls the first, middle and last function of each
# file, so a truncated stream leaves the last one undefined and the build
# fails; the interpolation's sum is checked exactly.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] source_size_unbounded: $AE not built"
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/bigmod"

MAIN_N=6000
MOD_N=9000
TERMS=150

awk -v n="$MOD_N" 'BEGIN {
    printf "exports("
    for (i = 0; i < n; i++) printf "%sadd_%d", (i ? ", " : ""), i
    print ")"
    for (i = 0; i < n; i++) printf "add_%d(x: int) -> int { return x + %d }\n", i, i
}' > "$WORK/bigmod/module.ae"

awk -v n="$MAIN_N" -v m="$MOD_N" -v terms="$TERMS" 'BEGIN {
    print "import bigmod"
    for (i = 0; i < n; i++) printf "f%d(x: int) -> int { return x + %d }\n", i, i
    print "main() {"
    printf "    a = f0(1) + f%d(1) + f%d(1)\n", int(n / 2), n - 1
    printf "    b = bigmod.add_0(1) + bigmod.add_%d(1) + bigmod.add_%d(1)\n", int(m / 2), m - 1
    for (i = 0; i <= terms; i++) printf "    x%d = %d\n", i, i
    printf "    println(\"a=${a} b=${b} s=${"
    for (i = 0; i <= terms; i++) printf "%sx%d * 1", (i ? " + " : ""), i
    print "}\")"
    print "}"
}' > "$WORK/main.ae"

# 1 + 0, 1 + n/2, 1 + n-1  →  3 + 3n/2 - 1
want_a=$((3 + MAIN_N / 2 + MAIN_N - 1))
want_b=$((3 + MOD_N / 2 + MOD_N - 1))
want_s=$((TERMS * (TERMS + 1) / 2))
want="a=$want_a b=$want_b s=$want_s"

OUT="$(AETHER_HOME="$ROOT" "$AE" run "$WORK/main.ae" 2>&1)"
RC=$?
if [ $RC -ne 0 ]; then
    echo "  [FAIL] source_size_unbounded: the program did not compile/run (rc=$RC)"
    echo "$OUT" | head -8
    exit 1
fi
if echo "$OUT" | grep -Fxq "$want"; then
    echo "  [PASS] source_size_unbounded: main file, module and interpolation past every old token cap"
else
    echo "  [FAIL] source_size_unbounded: expected '$want'"
    echo "$OUT" | head -8
    exit 1
fi
