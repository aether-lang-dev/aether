#!/bin/sh
# Regression (#2897): a clause set's codegen grows near linearly with its
# size, and a set past 64 clauses gives every clause the same answers.
#
# A set of 2000 clauses compiled in about 4 s and one of 5000 in about
# 79 s. Five passes walked the set, or the whole program, per clause:
#   - the heap-return classification memoised its answer on the first 64
#     clauses only, so each return of each later clause classified the set
#     again, and each of those walks looked up the clause a body belongs to
#     with a scan of the set (cubic);
#   - "which set is this definition one of" scanned the set, asked for each
#     clause and each name a body reads;
#   - "which clause of the set is @c_callback" scanned the set per clause;
#   - each clause's parameter types scanned the set, per clause function
#     and per dispatcher test;
#   - a set returning a tuple walked the whole program once per clause to
#     type the calls.
# Each is answered from an index or a memo now: 8000 clauses compile in
# under half a second, to the same C.
#
# The timing half is a loose bound, not a benchmark: 6000 clauses of each
# shape get 60 s, where the fix takes well under 1 s and the cubic passes
# took minutes, so a loaded runner does not flake it. The functional half
# runs sets of 300 clauses, past the 64 the memo held, and checks what the
# first, a late and the catch-all clause return.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
EXE=""
[ -x "$ROOT/build/ae" ] || EXE=".exe"
AE="$ROOT/build/ae$EXE"
AETHERC="$ROOT/build/aetherc$EXE"
if [ ! -x "$AE" ] || [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] clause_set_scaling: compiler not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0

# gen <shape> <n> <name>: a clause set `<name>` of n clauses and a catch-all.
gen() {
    awk -v shape="$1" -v n="$2" -v f="$3" 'BEGIN {
        for (i = 0; i < n; i++) {
            if (shape == "int")   printf "%s(%d) -> %d\n", f, i, i * 2
            if (shape == "heap")  printf "%s(%d) -> \"${%d}\"\n", f, i, i
            if (shape == "guard") printf "%s(k: int) when k == %d -> %d\n", f, i, i * 3
            if (shape == "tuple") printf "%s(%d) -> (int, string) {\n    return %d, \"${%d}\"\n}\n", f, i, i, i
        }
        if (shape == "int" || shape == "guard") printf "%s(k: int) -> k\n\n", f
        if (shape == "heap") printf "%s(k: int) -> \"other\"\n\n", f
        if (shape == "tuple") printf "%s(k: int) -> (int, string) {\n    return k, \"other\"\n}\n\n", f
    }'
}

# Timing: aetherc alone, 6000 clauses per shape, at most 60 s each. A
# portable watchdog: macOS has no timeout(1).
LIMIT=60
for shape in int heap guard tuple; do
    { gen "$shape" 6000 f; printf 'main() {\n    println("x")\n}\n'; } > "$TMP/big_$shape.ae"
    "$AETHERC" "$TMP/big_$shape.ae" "$TMP/big_$shape.c" > "$TMP/big_$shape.log" 2>&1 &
    pid=$!
    waited=0
    while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt "$LIMIT" ]; do
        sleep 1
        waited=$((waited + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
        echo "  [FAIL] clause_set_scaling: 6000 $shape clauses took over ${LIMIT} s"
        fail=1
        continue
    fi
    if ! wait "$pid"; then
        echo "  [FAIL] clause_set_scaling: 6000 $shape clauses did not compile"
        head -5 "$TMP/big_$shape.log" | sed 's/^/        /'
        fail=1
    fi
done

# Function: 300 clauses per shape, past the 64 the memo held.
{
    gen int 300 fi
    gen heap 300 fh
    gen guard 300 fg
    gen tuple 300 ft
    cat <<'AE'
main() {
    println("int ${fi(0)} ${fi(3)} ${fi(299)} ${fi(305)}")
    i = 0
    while i < 3 {
        println("heap ${fh(i * 140)} ${fh(305)}")
        i = i + 1
    }
    println("guard ${fg(1)} ${fg(250)} ${fg(400)}")
    a, b = ft(200)
    c, d = ft(305)
    println("tuple ${a} ${b} ${c} ${d}")
}
AE
} > "$TMP/sets.ae"
if ! ( cd "$TMP" && "$AE" build sets.ae -o "$TMP/sets" ) > "$TMP/sets.log" 2>&1; then
    echo "  [FAIL] clause_set_scaling: the 300-clause sets did not build"
    grep -m5 -E "error|Error" "$TMP/sets.log" | cut -c1-200 | sed 's/^/        /'
    fail=1
else
    got="$("$TMP/sets$EXE" 2>&1 | tr -d '\r')"
    want="int 0 6 598 305
heap 0 other
heap 140 other
heap 280 other
guard 3 750 400
tuple 200 200 305 other"
    if [ "$got" != "$want" ]; then
        echo "  [FAIL] clause_set_scaling: the 300-clause sets printed:"
        echo "$got" | sed 's/^/        /'
        fail=1
    fi
fi

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "  [PASS] clause_set_scaling: 6000-clause sets compile within ${LIMIT} s; 300-clause sets answer from every clause"
exit 0
