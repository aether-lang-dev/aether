#!/bin/sh
# #1986: an element access on a packed array is a load, not a call.
#
# `floatarr_get_unchecked(a, i)` was an out-of-line function in libaether,
# so every element of a hot loop paid a call the C compiler could not see
# through: no folding, no vectorisation. A 256x256 double matmul ran 19.6 ms
# through the accessors against 2.9 ms for the same loop over a raw buffer —
# 6.8x, and the root cause LangArena's Matmul/Nbody/NeuralNet cluster
# reported. The accessors are `static inline` in a header now, and the three
# modules pull it into the generated translation unit with
# `@c_include("aether_arr_inline.h")`.
#
# This test asserts the MECHANISM — the header reaches the generated C, no
# competing prototype is emitted, and the results match either way — and
# REPORTS the ratio between the two loops rather than asserting it: a shared
# runner can stall either loop for longer than the difference, and a gate
# that fails on that teaches people to re-run it. The ratio is in the log of
# whichever run regresses.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] packed_array_inline_access: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
import std.floatarr
import std.intarr
import std.os

extern malloc(n: int) -> ptr
extern free(p: ptr)

// The matmul inner loop, once through the packed-array accessors and once
// through an Aether float[] view over a raw buffer — the same arithmetic.
matmul_arr(a: ptr, b: ptr, c: ptr, n: int) {
    i = 0
    while i < n {
        k = 0
        while k < n {
            aik = floatarr.floatarr_get_unchecked(a, i * n + k)
            j = 0
            while j < n {
                cur = floatarr.floatarr_get_unchecked(c, i * n + j)
                floatarr.floatarr_set_unchecked(c, i * n + j,
                    cur + aik * floatarr.floatarr_get_unchecked(b, k * n + j))
                j = j + 1
            }
            k = k + 1
        }
        i = i + 1
    }
}

matmul_view(a: float[], b: float[], c: float[], n: int) {
    i = 0
    while i < n {
        k = 0
        while k < n {
            aik = a[i * n + k]
            j = 0
            while j < n {
                c[i * n + j] = c[i * n + j] + aik * b[k * n + j]
                j = j + 1
            }
            k = k + 1
        }
        i = i + 1
    }
}

main() {
    n = 128
    fa = floatarr.floatarr_new_raw(n * n)
    fb = floatarr.floatarr_new_raw(n * n)
    fc = floatarr.floatarr_new_raw(n * n)
    ra = malloc(n * n * 8)
    rb = malloc(n * n * 8)
    rc = malloc(n * n * 8)
    va = ra as float[]
    vb = rb as float[]
    vc = rc as float[]
    i = 0
    while i < n * n {
        v = ((i % 7) + 1) as float
        w = ((i % 3) + 1) as float
        floatarr.floatarr_set_unchecked(fa, i, v)
        floatarr.floatarr_set_unchecked(fb, i, w)
        floatarr.floatarr_set_unchecked(fc, i, 0.0)
        va[i] = v
        vb[i] = w
        vc[i] = 0.0
        i = i + 1
    }

    t0 = os.now_monotonic_ns()
    matmul_arr(fa, fb, fc, n)
    t1 = os.now_monotonic_ns()
    matmul_view(va, vb, vc, n)
    t2 = os.now_monotonic_ns()

    arr_ns = (t1 - t0) as float
    view_ns = (t2 - t1) as float
    // Same results, whichever way the elements were reached.
    same = floatarr.floatarr_get_unchecked(fc, 1234) == vc[1234]
    println("same ${same}")
    // The timing half is REPORTED, not asserted: a shared CI runner can
    // stall either loop for longer than the difference being measured, and
    // a gate that fails on that teaches people to re-run it. The mechanism
    // (the header in the TU, no competing prototype) is asserted below and
    // is what makes the ratio what it is; this line is here so a regression
    // shows up in the log of the run that caused it.
    if view_ns > 0.0 {
        println("ratio ${arr_ns / view_ns} (was ~6.8 before the accessors inlined)")
    } else {
        println("ratio unmeasured (the view loop took no measurable time)")
    }

    // The int twin, so the same mechanism is exercised for intarr.
    ia = intarr.intarr_new_raw(16)
    intarr.intarr_set_unchecked(ia, 3, 42)
    println("int ${intarr.intarr_get_unchecked(ia, 3)}")
    intarr.intarr_free(ia)

    floatarr.floatarr_free(fa)
    floatarr.floatarr_free(fb)
    floatarr.floatarr_free(fc)
    free(ra)
    free(rb)
    free(rc)
}
AE

out="$("$AE" run "$tmp/main.ae" 2>&1 | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
for want in "same true" "int 42"; do
    if ! printf '%s\n' "$out" | grep -q "^$want$"; then
        echo "  [FAIL] packed_array_inline_access: expected '$want'"
        printf '%s\n' "$out" | head -5 | sed 's/^/        /'
        fail=1
    fi
done

# The mechanism: the header is included and the accessor's body is in the
# translation unit, so the C compiler can see through the access.
"$AETHERC" "$tmp/main.ae" "$tmp/out.c" >/dev/null 2>&1
if ! printf '%s
' "$out" | grep -q "^ratio "; then
    echo "  [FAIL] packed_array_inline_access: the timing line is missing"
    fail=1
fi
if ! grep -q '#include "aether_arr_inline.h"' "$tmp/out.c"; then
    echo "  [FAIL] packed_array_inline_access: the module's @c_include did not reach the generated C"
    fail=1
fi
# ... and Aether emits no competing prototype for them (@c_import).
if grep -q '^double floatarr_get_unchecked' "$tmp/out.c"; then
    echo "  [FAIL] packed_array_inline_access: a duplicate prototype was emitted for an inline accessor"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] packed_array_inline_access: the accessors inline into the caller's TU; $(printf '%s
' "$out" | grep '^ratio ')"
fi
exit $fail
