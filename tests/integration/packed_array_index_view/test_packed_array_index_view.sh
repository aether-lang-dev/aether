#!/bin/sh
# #2041: `v[i]` on a packed array, through a typed view.
#
# The ask was `a[i] = v` instead of
# `intarr.intarr_set_unchecked(a, i, v)` — every element access in an
# array-heavy port is a function call otherwise, and the LangArena port
# named that as the ergonomic cost. The issue recorded the blocker: the
# handle is a bare `ptr`, so `[]` has no element type to dispatch on, and
# the general fix was assumed to be a distinct handle type threaded through
# three module APIs and every caller.
#
# It is not needed. A VIEW is typed, so `[]` already works on one:
#
#     v = intarr.intarr_array(a)
#     v[3] = 42
#
# `std.strarr` has had exactly this shape since it was written
# (`strarr.array` feeding `sort.strings_by`). The view is the buffer, not a
# copy, so writes through it are writes to the array — which is what makes
# it the answer rather than a consolation prize.
#
# This asserts that, that the view and the accessors see the same memory in
# both directions, and that indexing lowers to a load rather than to a call
# (the point of #1986's inlining would be lost if the sugar cost a call).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

[ -x "$AE" ] || { echo "  [SKIP] packed_array_index_view: ae not built"; exit 0; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
import std.intarr
import std.floatarr
import std.longarr

main() {
    a = intarr.intarr_new_raw(8)
    v = intarr.intarr_array(a)

    // Written through the view ...
    v[3] = 42
    v[4] = v[3] * 2
    // ... and read back through the accessor: the same memory.
    println("int ${v[3]} ${v[4]} ${intarr.intarr_get_unchecked(a, 4)}")

    // ... and the other way round.
    intarr.intarr_set_unchecked(a, 5, 7)
    println("both ${v[5]}")
    intarr.intarr_free(a)

    f = floatarr.floatarr_new_raw(4)
    fv = floatarr.floatarr_array(f)
    fv[1] = 1.5
    println("float ${fv[1] + 0.5} ${floatarr.floatarr_get_unchecked(f, 1)}")
    floatarr.floatarr_free(f)

    l = longarr.longarr_new_raw(4)
    lv = longarr.longarr_array(l)
    lv[2] = 9000000000
    println("long ${lv[2]} ${longarr.longarr_get_unchecked(l, 2)}")
    longarr.longarr_free(l)
}
AE

out="$("$AE" run "$tmp/main.ae" 2>&1 | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
expected="int 42 84 84
both 7
float 2 1.5
long 9000000000 9000000000"
if [ "$out" != "$expected" ]; then
    echo "  [FAIL] packed_array_index_view: unexpected results"
    printf 'got:\n%s\nwant:\n%s\n' "$out" "$expected" | sed 's/^/        /'
    fail=1
fi

# The sugar must not cost a call. `v[i]` lowers to a subscript in the
# generated C, and the view accessor itself is inline in the header -- if
# either became an out-of-line call, #1986's whole point would be lost and
# this would still "work".
"$AETHERC" "$tmp/main.ae" "$tmp/out.c" >/dev/null 2>&1
if ! grep -q '#include "aether_arr_inline.h"' "$tmp/out.c"; then
    echo "  [FAIL] packed_array_index_view: the inline header did not reach the generated C"
    fail=1
fi
if grep -qE '^(int|double|long long)\* +intarr_data' "$tmp/out.c"; then
    echo "  [FAIL] packed_array_index_view: a competing out-of-line prototype was emitted for the view accessor"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] packed_array_index_view: v[i] reads and writes the array itself, for int/float/long, with no call in the way"
fi
exit $fail
