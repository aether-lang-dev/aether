#!/bin/sh
# #3537: the checker's lookups by name of a file's own externs, of the
# program's own top-level vars and of what a module declares are probes of
# an index, not walks of the program, so checking a program grows with its
# size, not with the square of it.
#
# A module of N externs, N vars and N functions, each function calling four
# of the externs and reading four of the vars, beside a program of N vars of
# its own: every call in a merged body that resolved to an extern walked the
# module's top level for the file's own declaration (and searched a list of
# the symbols made for them), and every read of a var walked the program's
# vars. The check (aetherc --time-phases, its typecheck phase) is timed at
# 1000 and at 4000: four times the declarations must cost less than eight
# times the time (linear is four, the walks made it twelve).
#
# The time is the CPU time aetherc spent, summed over three runs of each
# size taken in turn, not the wall clock: the sweep runs this beside other
# tests, many of them running a scheduler on every core, and the wall clock
# of the longer compile counted the time it waited for a core. A linear
# check read 8.9x on a busy Windows runner and 4.8x alone.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
[ -x "$AETHERC" ] || [ ! -x "$AETHERC.exe" ] || AETHERC="$AETHERC.exe"
NAME=lookup_index_scaling

if [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] $NAME: aetherc not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

# program <n>: the program and its module in $TMPDIR/p<n>.
program() {
    d="$TMPDIR/p$1"
    mkdir -p "$d/lib/big"
    awk -v n="$1" 'BEGIN {
        for (i = 0; i < n; i++) print "extern big_ext" i "(x: int) -> int"
        for (i = 0; i < n; i++) print "var mv" i " = " i
        for (i = 0; i < n; i++) {
            a = (i + 1) % n; b = (i + 2) % n; c = (i + 3) % n
            print ""
            print "fn" i "(x: int) -> int {"
            print "    return big_ext" i "(x) + big_ext" a "(x) + big_ext" b "(x) + big_ext" c "(x) + mv" i " + mv" a " + mv" b " + mv" c
            print "}"
        }
    }' > "$d/lib/big/module.ae"
    awk -v n="$1" 'BEGIN {
        print "import big"
        print ""
        for (i = 0; i < n; i++) print "var pv" i " = " i
        print ""
        print "main() {"
        print "    println(\"${big.fn0(1)} ${pv0}\")"
        print "}"
    }' > "$d/main.ae"
}

# check_ms <n>: compile the program of n; its typecheck phase's CPU time in
# whole ms in $MS.
check_ms() {
    d="$TMPDIR/p$1"
    (cd "$d" && "$AETHERC" --time-phases main.ae out.c) > "$d/aetherc.log" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "  [FAIL] $NAME: aetherc failed (exit $rc) on the program of $1"
        tail -5 "$d/aetherc.log" | sed 's/^/    /'
        exit 1
    fi
    MS=$(sed -n 's/^phase typecheck [0-9.]* \([0-9]*\)\.[0-9]*$/\1/p' "$d/aetherc.log")
    if [ -z "$MS" ]; then
        echo "  [FAIL] $NAME: no typecheck phase in aetherc's --time-phases output"
        exit 1
    fi
}

program 1000
program 4000
small=0
large=0
for run in 1 2 3; do
    check_ms 1000
    small=$(( small + MS ))
    check_ms 4000
    large=$(( large + MS ))
done

# Below 30 ms a run the clock's grain decides: Windows charges CPU time a
# clock tick, about 15.6 ms, at a time.
[ "$small" -lt 90 ] && small=90
if [ "$large" -ge $(( small * 8 )) ]; then
    echo "  [FAIL] $NAME: checked 1000 declarations in ${small} ms of CPU, 4000 in ${large} ms (three runs each): four times the declarations cost $(( large / small ))x the time (linear is 4x)"
    exit 1
fi
echo "  [PASS] $NAME: checked 1000 declarations in ${small} ms of CPU, 4000 in ${large} ms (three runs each)"
exit 0
