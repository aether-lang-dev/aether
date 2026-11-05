#!/bin/sh
# #3546: the module merge and the code generator's extern lookups are probes
# of an index, not walks of a list, so merging a module and writing the
# calls of a program's externs grow with their size, not with its square.
#
# The merge scanned the program for every declaration it merged (is a
# function or a constant of this name there already?), the module for every
# name it renamed (does an extern of the build bind `<ns>_<name>`?), and the
# module's name lists for every identifier of every merged body. The code
# generator scanned the extern table, and the file's declarations, for every
# extern call. Each phase (aetherc --time-phases, its CPU column) is timed
# at 1500 and at 6000 declarations, past the 4096 the merge's lists once
# held (#3545): four times the declarations must cost less than eight times
# the time (linear is four; the scans made the merge 21 times and the code
# generator 13 times the time).
#
# The time is the CPU time aetherc spent, summed over three runs of each
# size taken in turn, not the wall clock, which a busy runner stretches
# (lookup_index_scaling).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
[ -x "$AETHERC" ] || [ ! -x "$AETHERC.exe" ] || AETHERC="$AETHERC.exe"
NAME=merge_codegen_scaling

if [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] $NAME: aetherc not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

# program <n>: in $TMPDIR/m<n>, a module of n functions, each calling two
# of its siblings and reading two of its n vars, and a program calling the
# one at the end of the chain; in $TMPDIR/c<n>, a program of n externs
# (std.string's C functions under names of their own), each called once.
program() {
    d="$TMPDIR/m$1"
    mkdir -p "$d/lib/big"
    awk -v n="$1" 'BEGIN {
        for (i = 0; i < n; i++) print "var mv" i " = " i
        for (i = 0; i < n; i++) {
            print ""
            print "fn" i "(x: int) -> int {"
            if (i < 2) print "    return x + mv" i
            else print "    return fn" (i - 1) "(x) + fn" (i - 2) "(x) + mv" i " + mv" (i - 1)
            print "}"
        }
    }' > "$d/lib/big/module.ae"
    awk -v n="$1" 'BEGIN {
        print "import big"
        print ""
        print "main() {"
        print "    println(\"${big.fn" (n - 1) "(1)}\")"
        print "}"
    }' > "$d/main.ae"
    d="$TMPDIR/c$1"
    mkdir -p "$d"
    awk -v n="$1" 'BEGIN {
        for (i = 0; i < n; i++) print "@extern(\"string_length\") len" i "(s: string) -> int"
        print ""
        print "main() {"
        print "    t = 0"
        for (i = 0; i < n; i++) print "    t = t + len" i "(\"ab\")"
        print "    println(\"${t}\")"
        print "}"
    }' > "$d/main.ae"
}

# phase_ms <dir> <phase>: compile <dir>/main.ae; the phase's CPU time in
# whole ms in $MS.
phase_ms() {
    (cd "$1" && "$AETHERC" --time-phases main.ae out.c) > "$1/aetherc.log" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "  [FAIL] $NAME: aetherc failed (exit $rc) on $1"
        tail -5 "$1/aetherc.log" | sed 's/^/    /'
        exit 1
    fi
    MS=$(sed -n "s/^phase $2 [0-9.]* \([0-9]*\)\.[0-9]*\$/\1/p" "$1/aetherc.log")
    if [ -z "$MS" ]; then
        echo "  [FAIL] $NAME: no $2 phase in aetherc's --time-phases output"
        exit 1
    fi
}

program 1500
program 6000
merge_small=0
merge_large=0
gen_small=0
gen_large=0
for run in 1 2 3; do
    phase_ms "$TMPDIR/m1500" merge
    merge_small=$(( merge_small + MS ))
    phase_ms "$TMPDIR/m6000" merge
    merge_large=$(( merge_large + MS ))
    phase_ms "$TMPDIR/c1500" codegen
    gen_small=$(( gen_small + MS ))
    phase_ms "$TMPDIR/c6000" codegen
    gen_large=$(( gen_large + MS ))
done

# Below 30 ms a run the clock's grain decides: Windows charges CPU time a
# clock tick, about 15.6 ms, at a time.
[ "$merge_small" -lt 90 ] && merge_small=90
[ "$gen_small" -lt 90 ] && gen_small=90
fail=0
if [ "$merge_large" -ge $(( merge_small * 8 )) ]; then
    echo "  [FAIL] $NAME: merged 1500 declarations in ${merge_small} ms of CPU, 6000 in ${merge_large} ms (three runs each): $(( merge_large / merge_small ))x the time for four times the declarations (linear is 4x)"
    fail=1
fi
if [ "$gen_large" -ge $(( gen_small * 8 )) ]; then
    echo "  [FAIL] $NAME: wrote 1500 extern calls in ${gen_small} ms of CPU, 6000 in ${gen_large} ms (three runs each): $(( gen_large / gen_small ))x the time for four times the calls (linear is 4x)"
    fail=1
fi
[ "$fail" = 0 ] || exit 1
echo "  [PASS] $NAME: merge 1500 / 6000 declarations ${merge_small} / ${merge_large} ms of CPU, codegen 1500 / 6000 extern calls ${gen_small} / ${gen_large} ms (three runs each)"
exit 0
