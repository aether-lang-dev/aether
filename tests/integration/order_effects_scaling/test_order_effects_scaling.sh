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
# runner. The chain is compiled at 4000 and at 16000 functions on the same
# machine: four times the functions must cost less than eight times the time
# (linear is four, the quadratic compile was thirteen), whatever the
# runner's speed.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
[ -x "$AETHERC" ] || [ ! -x "$AETHERC.exe" ] || AETHERC="$AETHERC.exe"

if [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] order_effects_scaling: aetherc not built"
    exit 0
fi
if ! perl -MTime::HiRes -e 1 2>/dev/null; then
    echo "  [SKIP] order_effects_scaling: no perl Time::HiRes to time the compiles"
    exit 0
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

# A sanity ceiling on the larger compile, far above what a linear one takes
# on any runner.
LIMIT_MS=60000

now_ms() { perl -MTime::HiRes=time -e 'printf "%d\n", time * 1000'; }

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

# compile_ms <n>: compile the chain of n functions; its time in ms in $MS.
compile_ms() {
    start=$(now_ms)
    "$AETHERC" "$TMPDIR/chain$1.ae" "$TMPDIR/chain$1.c" > "$TMPDIR/aetherc$1.log" 2>&1
    rc=$?
    MS=$(( $(now_ms) - start ))
    if [ "$rc" -ne 0 ]; then
        echo "  [FAIL] order_effects_scaling: aetherc failed (exit $rc) on a chain of $1 calls"
        tail -5 "$TMPDIR/aetherc$1.log" | sed 's/^/    /'
        exit 1
    fi
}

chain 4000
chain 16000
# The best of three of each, taken in turn, so a moment the runner is busy
# slows one run, not the comparison (a single run each on macOS ARM64 once
# measured 220 ms and 3263 ms, 14x, where this machine measures 3.7x).
small=""
large=""
for run in 1 2 3; do
    compile_ms 4000
    if [ -z "$small" ] || [ "$MS" -lt "$small" ]; then small=$MS; fi
    compile_ms 16000
    if [ -z "$large" ] || [ "$MS" -lt "$large" ]; then large=$MS; fi
done

if [ "$large" -gt "$LIMIT_MS" ]; then
    echo "  [FAIL] order_effects_scaling: a chain of 16000 calls took ${large} ms to compile (ceiling ${LIMIT_MS} ms)"
    exit 1
fi
# Below 50 ms the timer's grain and the process start decide; both are fast.
[ "$small" -lt 50 ] && small=50
if [ "$large" -ge $(( small * 8 )) ]; then
    echo "  [FAIL] order_effects_scaling: 4000 calls compiled in ${small} ms, 16000 in ${large} ms: four times the functions cost $(( large / small ))x the time (linear is 4x)"
    exit 1
fi
# The read of q.n comes first, ahead of the call that writes it.
if ! grep -q "_eo[0-9]* = \|_AE_SEQ_LET" "$TMPDIR/chain16000.c"; then
    echo "  [FAIL] order_effects_scaling: q.n is not evaluated ahead of f0(p, 3)"
    exit 1
fi
echo "  [PASS] order_effects_scaling: 4000 calls in ${small} ms, 16000 in ${large} ms"
exit 0
