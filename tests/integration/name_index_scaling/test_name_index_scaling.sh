#!/bin/sh
# #3835: the checker's and the code generator's questions about the
# program's top-level names (is NAME a constant, a module `var`, a constant
# with a value the evaluator can fold, an enum member; is it one of the
# module vars; which entry constant does a reference rename to) are probes
# of an index, not walks of program->children, so compiling a program
# grows with its size, not with the square of it.
#
# A program of N constants, N module vars and N functions, each function
# declaring two locals from two constants and a var and folding a condition
# (`if x > 1000000`, which the missing-return analysis asks the evaluator
# about): every declaration walked the program for a var of its name, every
# folded name walked it for a constant and then for an enum, and every
# reference walked the entry constants and the module vars. On Windows,
# checking took 78, 313 and 1469 ms of CPU for N = 1000, 2000 and 4000 (x4.3
# a doubling), and the code generator 63, 188 and 641 ms; with the index,
# 31, 63 and 156 ms and 31, 94 and 156 ms.
#
# Typecheck and codegen (aetherc --time-phases) are timed at 1000 and at
# 8000, the CPU time, fastest of three runs of each size taken in turn (see
# lookup_index_scaling for why CPU time and why the fastest): eight times
# the declarations must cost less than 24 times the time in each phase.
# Linear is eight; the walks made it about 45 for codegen and over 100 for
# typecheck.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
[ -x "$AETHERC" ] || [ ! -x "$AETHERC.exe" ] || AETHERC="$AETHERC.exe"
NAME=name_index_scaling

if [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] $NAME: aetherc not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

# program <n>: the program in $TMPDIR/p<n>.ae.
program() {
    awk -v n="$1" 'BEGIN {
        for (i = 0; i < n; i++) print "const C" i " = " i
        for (i = 0; i < n; i++) print "var g" i " = " i
        for (i = 0; i < n; i++) {
            a = (i + 1) % n; b = (i + 7) % n
            print ""
            print "f" i "(x: int) -> int {"
            print "    y = x + C" i " + C" a
            print "    z = y * 2 + g" b
            print "    if x > 1000000 {"
            print "        return f" a "(z)"
            print "    }"
            print "    return z"
            print "}"
        }
        print ""
        print "main() {"
        print "    println(\"${f0(1)}\")"
        print "}"
    }' > "$TMPDIR/p$1.ae"
}

# phase_ms <n>: compile the program of n; its typecheck and codegen phases'
# CPU time in whole ms in $TC and $CG.
phase_ms() {
    (cd "$TMPDIR" && "$AETHERC" --time-phases "p$1.ae" "p$1.c") > "$TMPDIR/aetherc$1.log" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "  [FAIL] $NAME: aetherc failed (exit $rc) on the program of $1"
        tail -5 "$TMPDIR/aetherc$1.log" | sed 's/^/    /'
        exit 1
    fi
    TC=$(sed -n 's/^phase typecheck [0-9.]* \([0-9]*\)\.[0-9]*$/\1/p' "$TMPDIR/aetherc$1.log")
    CG=$(sed -n 's/^phase codegen [0-9.]* \([0-9]*\)\.[0-9]*$/\1/p' "$TMPDIR/aetherc$1.log")
    if [ -z "$TC" ] || [ -z "$CG" ]; then
        echo "  [FAIL] $NAME: no typecheck or codegen phase in aetherc's --time-phases output"
        exit 1
    fi
}

program 1000
program 8000
tc_small=; tc_large=; cg_small=; cg_large=
for run in 1 2 3; do
    phase_ms 1000
    { [ -z "$tc_small" ] || [ "$TC" -lt "$tc_small" ]; } && tc_small=$TC
    { [ -z "$cg_small" ] || [ "$CG" -lt "$cg_small" ]; } && cg_small=$CG
    phase_ms 8000
    { [ -z "$tc_large" ] || [ "$TC" -lt "$tc_large" ]; } && tc_large=$TC
    { [ -z "$cg_large" ] || [ "$CG" -lt "$cg_large" ]; } && cg_large=$CG
done

# Below 30 ms a run the clock's grain decides: Windows charges CPU time a
# clock tick, about 15.6 ms, at a time.
[ "$tc_small" -lt 30 ] && tc_small=30
[ "$cg_small" -lt 30 ] && cg_small=30
fails=0
for phase in typecheck codegen; do
    if [ "$phase" = typecheck ]; then small=$tc_small; large=$tc_large; else small=$cg_small; large=$cg_large; fi
    if [ "$large" -ge $(( small * 24 )) ]; then
        echo "  [FAIL] $NAME: $phase took ${small} ms of CPU at 1000 declarations of each kind, ${large} ms at 8000 (the fastest of three runs each): eight times the declarations cost $(( large / small ))x the time (linear is 8x, the bound 24x)"
        fails=1
    fi
done
[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] $NAME: typecheck ${tc_small} to ${tc_large} ms, codegen ${cg_small} to ${cg_large} ms of CPU for 1000 to 8000 declarations of each kind"
exit 0
