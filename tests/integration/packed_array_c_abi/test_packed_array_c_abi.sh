#!/bin/sh
# #2169: the packed arrays' unchecked accessors are still part of libaether's
# exported ABI, as well as being inline.
#
# #1986 made `intarr_get_unchecked` and friends `static inline` in
# aether_arr_inline.h -- the vectorisation win is real -- and in the same
# change removed their out-of-line definitions from the .c files. C outside
# the standard library that declared and called them no longer linked.
# aether-ui's GTK4 and Win32 backends do exactly that:
#
#     extern double floatarr_get_unchecked(void* arr, int i);
#
# and on 0.708.0 every program linking the toolkit failed with
# `undefined reference to floatarr_get_unchecked` -- ae3d's editor among
# them. An ABI break in a minor release.
#
# Both must hold, so this checks both: a C program that only DECLARES the
# symbols, the way aether-ui does, links and runs against libaether; and a
# TU that includes the header still gets the inline bodies, so the #1986
# win is not quietly given back to fix the link.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
CC="${CC:-gcc}"

[ -x "$AE" ] || { echo "  [SKIP] packed_array_c_abi: ae not built"; exit 0; }
command -v "$CC" >/dev/null 2>&1 || { echo "  [SKIP] packed_array_c_abi: no C compiler ($CC)"; exit 0; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0
export AETHER_HOME="$ROOT"

# 1. The aether-ui shape: declarations only, no header, `void*` handles.
cat > "$tmp/consumer.c" <<'C'
#include <stdio.h>
#include <stdint.h>

extern void*    floatarr_new_raw(int size);
extern void     floatarr_free(void* arr);
extern double   floatarr_get_unchecked(void* arr, int i);
extern void     floatarr_set_unchecked(void* arr, int i, double value);
extern double*  floatarr_data(void* arr);

extern void*    intarr_new_raw(int size);
extern void     intarr_free(void* arr);
extern int      intarr_get_unchecked(void* arr, int i);
extern void     intarr_set_unchecked(void* arr, int i, int value);
extern int*     intarr_data(void* arr);

extern void*    longarr_new_raw(int size);
extern void     longarr_free(void* arr);
extern int64_t  longarr_get_unchecked(void* arr, int i);
extern void     longarr_set_unchecked(void* arr, int i, int64_t value);
extern int64_t* longarr_data(void* arr);

int main(void) {
    void* f = floatarr_new_raw(4);
    floatarr_set_unchecked(f, 2, 1.5);
    double* fv = floatarr_data(f);
    printf("float %g %g\n", floatarr_get_unchecked(f, 2), fv[2]);
    floatarr_free(f);

    void* a = intarr_new_raw(4);
    intarr_set_unchecked(a, 1, 42);
    printf("int %d %d\n", intarr_get_unchecked(a, 1), intarr_data(a)[1]);
    intarr_free(a);

    void* l = longarr_new_raw(4);
    longarr_set_unchecked(l, 3, (int64_t)9000000000LL);
    printf("long %lld %lld\n", (long long)longarr_get_unchecked(l, 3),
           (long long)longarr_data(l)[3]);
    longarr_free(l);
    return 0;
}
C

libs="$("$AE" cflags --libs 2>/dev/null)"
if ! "$CC" "$tmp/consumer.c" -o "$tmp/consumer" $libs > "$tmp/link.log" 2>&1; then
    echo "  [FAIL] packed_array_c_abi: a C consumer that declares the accessors no longer links"
    grep -iE "undefined|error" "$tmp/link.log" | head -5 | sed 's/^/        /'
    fail=1
else
    # Captured to a file and CR-stripped separately: piping straight into
    # `tr` would make the pipeline's status tr's, and a consumer that
    # crashed would read as wrong output rather than as a crash. (A Windows
    # binary writes CRLF; the comparison is about the values.)
    "$tmp/consumer" > "$tmp/run.out" 2>&1
    rc=$?
    out="$(tr -d '\r' < "$tmp/run.out")"
    if [ "$rc" -ne 0 ]; then
        echo "  [FAIL] packed_array_c_abi: the C consumer exited $rc"
        printf '%s' "$out" | head -5 | sed 's/^/        /'
        fail=1
    fi
    expected="float 1.5 1.5
int 42 42
long 9000000000 9000000000"
    if [ "$out" != "$expected" ]; then
        echo "  [FAIL] packed_array_c_abi: the exported accessors returned the wrong values"
        printf 'got:\n%s\n' "$out" | sed 's/^/        /'
        fail=1
    fi
fi

# 2. Every accessor the header declares has an exported twin in the library.
#    Checked against the header, so an accessor added there later without
#    its out-of-line definition is caught here rather than by a downstream
#    link.
hdr="$ROOT/std/collections/aether_arr_inline.h"
lib="$ROOT/build/libaether.a"
if command -v nm >/dev/null 2>&1 && [ -f "$lib" ]; then
    # Every accessor is written once, prefixed by its family's AETHER_ARR_*_FN
    # linkage macro; that prefix is how they are found.
    names="$(grep -E '^AETHER_ARR_[A-Z]+_FN ' "$hdr" \
             | grep -oE '[a-z]+arr_[a-z_]+\(' | tr -d '(' | sort -u)"
    [ -n "$names" ] || { echo "  [FAIL] packed_array_c_abi: could not read the accessor list from the header"; fail=1; }
    # Mach-O `nm` prints C symbols with a leading underscore; strip it so
    # the comparison is about names, not the object format.
    exported="$(nm "$lib" 2>/dev/null | awk '$2 == "T" {n = $3; sub(/^_/, "", n); print n}')"
    for name in $names; do
        if ! printf '%s\n' "$exported" | grep -qx "$name"; then
            echo "  [FAIL] packed_array_c_abi: $name is declared in the header but not exported by libaether"
            fail=1
        fi
    done
fi

# 3. ... and including the header still gives the INLINE body. The exported
#    twins must not have displaced it: with them, a TU that includes the
#    header would compile a call where #1986 put a load.
cat > "$tmp/includer.c" <<'C'
#include "aether_arr_inline.h"
double read_one(AetherFloatArray* a, int i) { return floatarr_get_unchecked(a, i); }
C
"$CC" -O2 -S -I"$ROOT/std/collections" "$tmp/includer.c" -o "$tmp/includer.s" 2>"$tmp/inc.log"
if [ ! -s "$tmp/includer.s" ]; then
    echo "  [FAIL] packed_array_c_abi: a TU including the header does not compile"
    head -3 "$tmp/inc.log" | sed 's/^/        /'
    fail=1
elif grep -q 'floatarr_get_unchecked' "$tmp/includer.s"; then
    # Inlined, the accessor is a load and its name appears nowhere in the
    # assembly. Any reference at all -- a `call`, the `jmp` gcc emits for a
    # tail call at -O2, an arm `b`/`bl` -- means it was called. Matching only
    # `call` could never fail on the tail-call gcc actually produces.
    echo "  [FAIL] packed_array_c_abi: including the header now references the accessor instead of inlining it"
    grep 'floatarr_get_unchecked' "$tmp/includer.s" | head -3 | sed 's/^/        /'
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] packed_array_c_abi: a C consumer that only declares the accessors links and runs; every header accessor is exported; including the header still inlines"
fi
exit $fail
