#!/bin/sh
# Regression: the compiler's per-program indexes (#2007) answer the same
# questions the top-level scans did, at scale.
#
# Type checking and codegen grew quadratically in the number of functions
# because a handful of lookups -- symbol resolution at file scope, "is this
# identifier a @c_callback", clause counting for pattern-matched functions,
# "does this TU declare an extern of this name", the forward-declaration
# de-duplication -- each walked every top-level node once per use. They now
# go through a per-scope symbol hash and one program index.
#
# This is the functional half of that change: a generated program large
# enough that the indexes are the path taken (1600 functions, well past the
# 16-symbol threshold at which a scope is indexed) exercising every question
# the index answers -- ordinary calls, a multi-clause function whose clauses
# are far apart in the file, a @c_callback passed as a value, a user function
# that shadows a whitelisted builtin so the folder must not fold it -- and
# checking the answers. The timing half is the table in #2007; a wall-clock
# assertion would be a flake on a loaded runner, so it is not repeated here.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] compile_scaling: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
N=1600

awk -v n="$N" 'BEGIN {
    # A multi-clause function: its first clause is the first definition in
    # the file and its last clause is the last, so any clause lookup that
    # only found the nearest definition would miss one.
    print "pick(0) -> int { return 100 }"
    for (i = 0; i < n; i++) printf "f%d(a: int) -> int {\n    return a + %d\n}\n", i, i
    print "pick(1) -> int { return 200 }"
    print "pick(n: int) -> int { return n }"
    # A user function that shadows a whitelisted pure builtin: the constant
    # folder must call it, not fold string.from_int(41) to \"41\".
    print "string_from_int(v: int) -> string { return \"shadowed\" }"
    print "@c_callback"
    print "on_event(v: int) -> int { return v * 2 }"
    print "apply(cb: fn, v: int) -> int { return cb(v) }"
    print "main() {"
    print "    t = 0"
    for (i = 0; i < n; i++) printf "    t = t + f%d(%d)\n", i, i
    print "    println(\"sum=${t}\")"
    print "    println(\"pick=${pick(0)} ${pick(1)} ${pick(7)}\")"
    print "    println(\"shadow=${string_from_int(41)}\")"
    print "    println(\"callback=${apply(on_event, 21)}\")"
    print "}"
}' > "$tmp/big.ae"

OUT="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/big.ae" 2>&1)"
RC=$?
if [ $RC -ne 0 ]; then
    echo "  [FAIL] compile_scaling: the $N-function program did not compile/run"
    echo "$OUT" | head -8
    exit 1
fi

# sum of 2*i for i in [0, N) = N*(N-1)
want_sum=$((N * (N - 1)))
fail=0
for line in "sum=$want_sum" "pick=100 200 7" "shadow=shadowed" "callback=42"; do
    if ! echo "$OUT" | grep -Fxq "$line"; then
        echo "  [FAIL] compile_scaling: expected line missing: $line"
        fail=1
    fi
done
if [ "$fail" -ne 0 ]; then
    echo "$OUT" | tail -6
    exit 1
fi
echo "  [PASS] compile_scaling: $N-function program resolves calls, clauses, a callback and a shadowed builtin through the indexes"
