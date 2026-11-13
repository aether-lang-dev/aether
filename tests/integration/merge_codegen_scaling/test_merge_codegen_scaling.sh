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
# at 1500 and at 12000 declarations, past the 4096 the merge's lists once
# held (#3545): eight times the declarations must cost less than 24 times
# the time. Linear is eight; at four times the declarations the scans made
# the merge 21 times and the code generator 13 times the time, which at
# eight is far past 24.
#
# The time is the CPU time aetherc spent, the fastest of three runs of
# each size, taken in turn (a run a busy runner stalled counts for nothing),
# not the wall clock, which a busy runner stretches
# (lookup_index_scaling). CPU time moves too: four times the size against
# a bound of 8x left no room for a runner that slows the larger case
# (#3604); eight times and 24x leave a factor of two or more on each side.

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
program 12000
merge_small=
merge_large=
gen_small=
gen_large=
for run in 1 2 3; do
    phase_ms "$TMPDIR/m1500" merge
    { [ -z "$merge_small" ] || [ "$MS" -lt "$merge_small" ]; } && merge_small=$MS
    phase_ms "$TMPDIR/m12000" merge
    { [ -z "$merge_large" ] || [ "$MS" -lt "$merge_large" ]; } && merge_large=$MS
    phase_ms "$TMPDIR/c1500" codegen
    { [ -z "$gen_small" ] || [ "$MS" -lt "$gen_small" ]; } && gen_small=$MS
    phase_ms "$TMPDIR/c12000" codegen
    { [ -z "$gen_large" ] || [ "$MS" -lt "$gen_large" ]; } && gen_large=$MS
done

# Below 30 ms a run the clock's grain decides: Windows charges CPU time a
# clock tick, about 15.6 ms, at a time.
[ "$merge_small" -lt 30 ] && merge_small=30
[ "$gen_small" -lt 30 ] && gen_small=30
fail=0
if [ "$merge_large" -ge $(( merge_small * 24 )) ]; then
    echo "  [FAIL] $NAME: merged 1500 declarations in ${merge_small} ms of CPU, 12000 in ${merge_large} ms (the fastest of three runs each): $(( merge_large / merge_small ))x the time for eight times the declarations (linear is 8x, the bound 24x)"
    fail=1
fi
if [ "$gen_large" -ge $(( gen_small * 24 )) ]; then
    echo "  [FAIL] $NAME: wrote 1500 extern calls in ${gen_small} ms of CPU, 12000 in ${gen_large} ms (the fastest of three runs each): $(( gen_large / gen_small ))x the time for eight times the calls (linear is 8x, the bound 24x)"
    fail=1
fi
[ "$fail" = 0 ] || exit 1
echo "  [PASS] $NAME: merge 1500 / 12000 declarations ${merge_small} / ${merge_large} ms of CPU, codegen 1500 / 12000 extern calls ${gen_small} / ${gen_large} ms (the fastest of three runs each)"
exit 0
