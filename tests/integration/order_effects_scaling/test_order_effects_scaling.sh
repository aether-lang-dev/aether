#!/bin/sh
# #2813: what calling a function can change (order_fn_effects, the
# evaluation-order rule of #2478) is solved over the call graph in time
# linear in its size.
#
# A chain of functions, each calling the next and only the last writing
# through its pointer parameter, asked about once (`show(q.n, f0(p, 3))`):
# one fixed point over every reached function took a pass per level, 90 s
# for 8000 of them, and the recursive solve before it ran out of stack.
#
# #3499: the whole compile is linear too. Three lookups by name scanned the
# program once per call or per top-level node (the checker's callee and
# extern lookups, codegen's fn-pointer globals and shadowed names, the
# merge's import prefixes), so the compile still grew with the square of the
# chain: 8000 functions took 22 to 25 s on the release's macOS x86_64
# runner. The chain is compiled at 2000 and at 16000 functions on the same
# machine: eight times the functions must cost less than 24 times the time,
# whatever the runner's speed. Linear is eight; the quadratic compile was
# thirteen at four times the functions, about fifty at eight.
#
# The time is the CPU time of the compile's phases (`aetherc --time-phases`,
# its second column), summed over three runs of each size taken in turn,
# not the wall clock: the sweep runs this beside other tests, many running a
# scheduler on every core, and the longer compile's wall clock counted the
# time it waited for a core (8.2x once on a Linux runner). CPU time moves
# too: at four times the functions a linear compile read 8.3x, alone, on a
# Windows runner, against a bound of 8x (#3604). Eight times the size and a
# bound of 24x leave a factor of two or more on each side.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
[ -x "$AETHERC" ] || [ ! -x "$AETHERC.exe" ] || AETHERC="$AETHERC.exe"

if [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] order_effects_scaling: aetherc not built"
    exit 0
fi
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

# A sanity ceiling on the larger compile's CPU time, far above what a linear
# one takes on any runner.
LIMIT_MS=60000

# chain <n>: write the chain of n functions to $TMPDIR/chain<n>.ae.
chain() {
    awk -v n="$1" 'BEGIN {
        print "extern malloc(size: int) -> ptr"
        print ""
        print "struct Box {"
        print "    n: int"
        print "}"
        print ""
        print "show(a: int, b: int) -> int {"
        print "    return a * 1000 + b"
        print "}"
        for (i = 0; i < n; i++) {
            print ""
            print "f" i "(b: *Box, d: int) -> int {"
            if (i + 1 < n) {
                print "    return f" (i + 1) "(b, d + 1)"
            } else {
                print "    b.n = 5"
                print "    return 2"
            }
            print "}"
        }
        print ""
        print "main() {"
        print "    p = malloc(sizeof(Box)) as *Box"
        print "    q = p"
        print "    p.n = 1"
        print "    r = show(q.n, f0(p, 3))"
        print "    println(\"${r}\")"
        print "}"
    }' > "$TMPDIR/chain$1.ae"
}

# compile_ms <n>: compile the chain of n functions; the CPU time of its
# phases in whole ms in $MS.
compile_ms() {
    "$AETHERC" --time-phases "$TMPDIR/chain$1.ae" "$TMPDIR/chain$1.c" > "$TMPDIR/aetherc$1.log" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "  [FAIL] order_effects_scaling: aetherc failed (exit $rc) on a chain of $1 calls"
        tail -5 "$TMPDIR/aetherc$1.log" | sed 's/^/    /'
        exit 1
    fi
    MS=$(awk '/^phase [a-z]+ [0-9.]+ [0-9.]+$/ { s += $4; n++ } END { if (n) printf "%d\n", s }' "$TMPDIR/aetherc$1.log")
    if [ -z "$MS" ]; then
        echo "  [FAIL] order_effects_scaling: no phases in aetherc's --time-phases output"
        exit 1
    fi
}

chain 2000
chain 16000
small=0
large=0
for run in 1 2 3; do
    compile_ms 2000
    small=$(( small + MS ))
    compile_ms 16000
    if [ "$MS" -gt "$LIMIT_MS" ]; then
        echo "  [FAIL] order_effects_scaling: a chain of 16000 calls took ${MS} ms of CPU to compile (ceiling ${LIMIT_MS} ms)"
        exit 1
    fi
    large=$(( large + MS ))
done

# Below 50 ms a run the clock's grain decides: Windows charges CPU time a
# clock tick, about 15.6 ms, at a time.
[ "$small" -lt 150 ] && small=150
if [ "$large" -ge $(( small * 24 )) ]; then
    echo "  [FAIL] order_effects_scaling: 2000 calls compiled in ${small} ms of CPU, 16000 in ${large} ms (three runs each): eight times the functions cost $(( large / small ))x the time (linear is 8x, the bound 24x)"
    exit 1
fi
# The read of q.n comes first, ahead of the call that writes it.
if ! grep -q -E "_eo[0-9]* = |_AE_SEQ_LET" "$TMPDIR/chain16000.c"; then
    echo "  [FAIL] order_effects_scaling: q.n is not evaluated ahead of f0(p, 3)"
    exit 1
fi
echo "  [PASS] order_effects_scaling: 2000 calls in ${small} ms of CPU, 16000 in ${large} ms (three runs each)"
exit 0
