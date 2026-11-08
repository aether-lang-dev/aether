#!/bin/sh
# Issue #3113: an index into a fixed-size array `T[N]` is checked against N,
# as a slice index is against its length. The generated C was a plain
# `a[i]`, so `a[4] = 99` on an `int[3]` wrote into the stack frame and
# `at(a, 5)` read whatever lay past the array, exit 0. Every index is
# checked now: a read, a store, `+=`, `++` and `--`, through a local, a
# parameter, a struct field, an actor's state and a `const` table, with the
# slice's panic ("array index 4 out of range for length 3", the source line
# before it), which `try` catches. An index known when the program is
# compiled (a literal, a `const`, arithmetic over them) out of range is a
# compile error; a parameter named like a `const` is not one.
# Run on both C paths.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"
if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] array_index_checked: $AE not built"
    exit 0
fi
TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT
fail=0

# $1 = source basename, $2 = extended regex the diagnostic must match
check_rejected() {
    name="$1"; want="$2"
    if "$AE" build "$SCRIPT_DIR/$name.ae" -o "$TMPDIR/out" >"$TMPDIR/$name.log" 2>&1; then
        echo "  [FAIL] array_index_checked: $name.ae compiled (should be rejected)"
        fail=1; return
    fi
    if ! tr -d '\r' < "$TMPDIR/$name.log" | grep -qE "$want"; then
        echo "  [FAIL] array_index_checked: $name.ae rejected, but not for the stated reason"
        echo "         expected a diagnostic matching: $want"
        sed 's/^/    /' "$TMPDIR/$name.log" | head -8
        fail=1
    fi
}

check_rejected reject_literal \
    "array index 3 is out of range for 'int\[3\]', whose indexes are 0 through 2"
check_rejected reject_const \
    "array index 4 is out of range for 'int\[4\]', whose indexes are 0 through 3"
check_rejected reject_negative \
    "array index -1 is out of range for 'int\[3\]', whose indexes are 0 through 2"

for cc in default strict; do
    if [ "$cc" = strict ]; then
        export AE_CC="${CC:-cc} -DAETHER_GCC_COMPAT=0"
    else
        unset AE_CC
    fi

    # Each out-of-range index caught; the arrays beside it untouched.
    if ! "$AE" build "$SCRIPT_DIR/caught.ae" -o "$TMPDIR/caught" >"$TMPDIR/caught.build.log" 2>&1; then
        echo "  [FAIL] array_index_checked ($cc): caught.ae did not build"
        sed 's/^/    /' "$TMPDIR/caught.build.log" | head -8
        fail=1
        continue
    fi
    "$TMPDIR/caught" >"$TMPDIR/caught.run.log" 2>&1
    rc=$?
    tr -d '\r' < "$TMPDIR/caught.run.log" > "$TMPDIR/caught.txt"
    if [ $rc -ne 0 ] || grep -q 'NOT REACHED' "$TMPDIR/caught.txt"; then
        echo "  [FAIL] array_index_checked ($cc): an out-of-range index was not caught (rc=$rc)"
        sed 's/^/    /' "$TMPDIR/caught.txt" | head -12
        fail=1
        continue
    fi
    for want in \
        'caught: .*caught\.ae:44: array index 4 out of range for length 3' \
        'caught: .*caught\.ae:17: array index 5 out of range for length 3' \
        'caught: .*caught\.ae:[0-9]*: array index 3 out of range for length 3' \
        'caught: .*caught\.ae:[0-9]*: array index -1 out of range for length 3' \
        'caught: .*caught\.ae:21: array index 4 out of range for length 4' \
        'caught: .*caught\.ae:[0-9]*: array index 4 out of range for length 4' \
        '^a=11 3 14 b=7 8 9 c=40$' \
        '^stored 5$' \
        'caught: .*caught\.ae:30: array index 3 out of range for length 3'; do
        if ! grep -qE -- "$want" "$TMPDIR/caught.txt"; then
            echo "  [FAIL] array_index_checked ($cc): no line matching: $want"
            sed 's/^/    /' "$TMPDIR/caught.txt" | head -14
            fail=1
        fi
    done
    n=$(grep -c 'array index 3 out of range for length 3' "$TMPDIR/caught.txt")
    if [ "$n" -ne 3 ]; then
        echo "  [FAIL] array_index_checked ($cc): '+=', '++' and the actor's store: $n of 3 caught"
        fail=1
    fi

    # Uncaught, it is a panic: exit non-zero, the message on the way out.
    if ! "$AE" build "$SCRIPT_DIR/panic_shadowed.ae" -o "$TMPDIR/shadowed" >"$TMPDIR/shadowed.build.log" 2>&1; then
        echo "  [FAIL] array_index_checked ($cc): panic_shadowed.ae did not build"
        sed 's/^/    /' "$TMPDIR/shadowed.build.log" | head -8
        fail=1
        continue
    fi
    if "$TMPDIR/shadowed" >"$TMPDIR/shadowed.run.log" 2>&1; then
        echo "  [FAIL] array_index_checked ($cc): panic_shadowed exited 0 (should panic)"
        fail=1
    elif ! tr -d '\r' < "$TMPDIR/shadowed.run.log" |
            grep -qE 'panic_shadowed\.ae:6: array index 7 out of range for length 3'; then
        echo "  [FAIL] array_index_checked ($cc): panic_shadowed panicked, but not with the expected message"
        sed 's/^/    /' "$TMPDIR/shadowed.run.log" | head -8
        fail=1
    fi
done
unset AE_CC

[ $fail -eq 0 ] && echo "  [PASS] array_index_checked: every index into a T[N] checked on both C paths, try catches it, constant ones refused"
exit $fail
