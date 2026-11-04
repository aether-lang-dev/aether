#!/bin/sh
# #3574: the checks that follow a loop to a fixed point cost time linear in
# how deep loops nest.
#
# A loop's walk walks its body two or three times (its passes, the walk
# that reports, the free check's next turn), and each walk of the body
# walked the loops inside it again, so the cost doubled with each level:
# 20 nested loops took the free check, the slice checks, the isolation
# check and the protocol check seconds, and 70 did not finish. A loop
# walked again from the same state is now the walk already made: its
# answer, and the joins it made into the loops around it, are reused.
#
# Each function here nests its loops D deep, with a string, a view of an
# array, a protocol reference and a labeled `continue` and `break` of the
# outer loop in the innermost body, so every such check walks them. The
# program is compiled at D = 12 and D = 24: twice the depth must cost less
# than eight times the CPU time (linear is two; the doubling cost was
# thousands). The time is the CPU column of `aetherc --time-phases`, summed
# over three runs of each, and the program at D = 24 also runs.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
[ -x "$AETHERC" ] || [ ! -x "$AETHERC.exe" ] || AETHERC="$AETHERC.exe"
NAME=loop_nest_scaling
if [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] $NAME: aetherc not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
# A ceiling on one compile's CPU time, far above what a linear one takes,
# and one on its wall clock: the doubling compile took hours.
LIMIT_MS=60000
. "$ROOT/tests/lib/sweep_timeout.sh"
sweep_timeout_bound TO "$ROOT" 120
AETHER_HOME="$ROOT"
export AETHER_HOME
FUNCS=100

# nest <depth>: write $TMP/nest<depth>.ae, $FUNCS functions of loops nested
# <depth> deep, and a main that runs the first.
nest() {
    awk -v d="$1" -v k="$FUNCS" 'BEGIN {
        print "import std.string"
        print ""
        print "message Ping {}"
        print "protocol P { start -> Ping -> start }"
        print "actor A implements P { receive { Ping() -> {} } }"
        for (f = 0; f < k; f++) {
            print ""
            printf "nest%d(w: string) -> int {\n", f
            print "    let arr: int[4] = [1, 2, 3, 4]"
            print "    v = arr[0..2]"
            print "    a = spawn(A())"
            print "    hits = 0"
            print "    outer: while hits < 1000 {"
            for (i = 1; i < d; i++) printf "    l%d: while true {\n", i
            print "    s = string.concat(w, \"x\")"
            print "    hits = hits + string.length(s) + v[0]"
            print "    a ! Ping {}"
            print "    if hits < 9 {"
            print "        continue outer"
            print "    }"
            print "    break outer"
            for (i = 1; i < d; i++) print "    }"
            print "    }"
            print "    return hits"
            print "}"
        }
        print ""
        print "main() {"
        print "    n = nest0(string.concat(\"a\", \"bcd\"))"
        print "    wait_for_idle()"
        print "    println(\"hits ${n}\")"
        print "}"
    }' > "$TMP/nest$1.ae"
}

# compile_ms <depth>: MS is the CPU time of compiling nest<depth>.ae.
compile_ms() {
    $TO "$AETHERC" --time-phases "$TMP/nest$1.ae" "$TMP/nest$1.c" \
        > "$TMP/aetherc$1.log" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then
        if [ "$rc" -eq 124 ]; then
            echo "  [FAIL] $NAME: loops nested $1 deep did not compile in 120 s"
            exit 1
        fi
        echo "  [FAIL] $NAME: aetherc failed (exit $rc) on loops nested $1 deep"
        grep -E "error|rror:" "$TMP/aetherc$1.log" | head -5 | sed 's/^/    /'
        exit 1
    fi
    MS=$(awk '/^phase [a-z]+ [0-9.]+ [0-9.]+$/ { s += $4; n++ } END { if (n) printf "%d\n", s }' \
        "$TMP/aetherc$1.log")
    if [ -z "$MS" ]; then
        echo "  [FAIL] $NAME: no phases in aetherc's --time-phases output"
        exit 1
    fi
}

nest 12
nest 24
small=0
large=0
for run in 1 2 3; do
    compile_ms 12
    small=$(( small + MS ))
    compile_ms 24
    if [ "$MS" -gt "$LIMIT_MS" ]; then
        echo "  [FAIL] $NAME: loops nested 24 deep took ${MS} ms of CPU to compile (ceiling ${LIMIT_MS} ms)"
        exit 1
    fi
    large=$(( large + MS ))
done

# Below 50 ms a run the clock's grain decides: Windows charges CPU time a
# clock tick, about 15.6 ms, at a time.
[ "$small" -lt 150 ] && small=150
if [ "$large" -ge $(( small * 8 )) ]; then
    echo "  [FAIL] $NAME: loops nested 12 deep compiled in ${small} ms of CPU, 24 deep in ${large} ms (three runs each): twice the depth cost $(( large / small ))x the time (linear is 2x)"
    exit 1
fi

if [ -x "$AE" ] || [ -x "$AE.exe" ]; then
    out=$(cd "$TMP" && AETHER_HOME="$ROOT" "$AE" run nest24.ae 2>&1 | tr -d '\r' | tail -1)
    if [ "$out" != "hits 12" ]; then
        echo "  [FAIL] $NAME: loops nested 24 deep printed \"$out\", want \"hits 12\""
        exit 1
    fi
fi
echo "  [PASS] $NAME: loops nested 12 deep in ${small} ms of CPU, 24 deep in ${large} ms (three runs each)"
exit 0
