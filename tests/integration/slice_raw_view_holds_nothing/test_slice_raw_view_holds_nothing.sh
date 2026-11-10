#!/bin/sh
# #3583: a slice local only ever given views of memory no count covers (a
# cast of a pointer, `null`, an array literal, an array viewed as a slice, a
# sub-slice of one, a local that is one) holds no reference. Its function
# registers nothing for a caught panic and releases nothing at its exits,
# so `buf = c.data as float[]; return buf[i]` is an indexed load once the C
# compiler inlines the view, as it was before #3568. A local that may be
# given a view of a counted buffer on some path, directly or through
# another local, a parameter's view or a field's, still holds and releases
# its own. What runs is tests/regression/test_slice_raw_view_holds_nothing.ae's.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"

[ -x "$AETHERC" ] || [ -x "$AETHERC.exe" ] ||
    { echo "  [SKIP] slice_raw_view_holds_nothing: aetherc not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fails=0

cat > "$TMP/views.ae" <<'AE'
struct Column {
    data: ptr
    arity: int
}

struct Holder {
    data: int[]
}

column_get(c: *Column, slot: int, k: int) -> float {
    buf = c.data as float[]
    return buf[slot * c.arity + k]
}

column_set(c: *Column, slot: int, k: int, v: float) {
    buf = c.data as float[]
    buf[slot * c.arity + k] = v
}

raw_chain(p: ptr, n: int) -> int {
    first = (p as int[])[0..n]
    tail = first[1..n]
    again = tail
    again = p as int[]
    int[] lit = [1, 2, 3]
    arr = [4, 5, 6]
    part = arr[0..2]
    other = lit
    other = part
    return first[0] + tail[0] + again[0] + other[0]
}

mixed_make(p: ptr, take: int) -> int {
    xs = make([]int, 4)
    v = p as int[]
    if take > 0 {
        v = xs[1..3]
    }
    return v[0]
}

mixed_chain(p: ptr) -> int {
    xs = make([]int, 4)
    a = p as int[]
    b = a[0..2]
    a = xs[1..4]
    return b[0] + a[0]
}

mixed_param(xs: int[], p: ptr) -> int {
    v = p as int[]
    v = xs[0..1]
    return v[0]
}

mixed_field(h: Holder, p: ptr) -> int {
    v = p as int[]
    v = h.data
    return v[0]
}

main() {
}
AE

if ! "$AETHERC" "$TMP/views.ae" "$TMP/views.c" > "$TMP/out.txt" 2>&1; then
    echo "  [FAIL] slice_raw_view_holds_nothing: the program did not compile"
    sed 's/^/        /' "$TMP/out.txt"
    exit 1
fi

# body <name>: the generated definition of function <name>.
body() {
    awk -v name="$1" '
        $0 ~ "^[A-Za-z_][A-Za-z0-9_ *]* " name "\\(.*\\) \\{$" { on = 1 }
        on { print }
        on && /^}/ { exit }
    ' "$TMP/views.c"
}

# holds_none <name>: no registration for a caught panic and no release.
holds_none() {
    b=$(body "$1")
    if [ -z "$b" ]; then
        echo "  [FAIL] $1: no definition in the generated C"
        fails=$((fails + 1))
        return
    fi
    case "$b" in
        *aether_unwind_track*|*aether_slice_release*|*aether_slice_set*)
            echo "  [FAIL] $1: a local of views of uncounted memory holds a reference"
            echo "$b" | grep -n -E "aether_unwind_track|aether_slice_release|aether_slice_set" |
                sed 's/^/        /'
            fails=$((fails + 1)) ;;
    esac
}

# holds_own <name>: the local that may view a counted buffer is released.
holds_own() {
    b=$(body "$1")
    case "$b" in
        *aether_slice_release*) ;;
        *) echo "  [FAIL] $1: a local that may view a counted buffer releases nothing"
           fails=$((fails + 1)) ;;
    esac
}

holds_none column_get
holds_none column_set
holds_none raw_chain
holds_own mixed_make
holds_own mixed_chain
holds_own mixed_param
holds_own mixed_field

if [ "$fails" -eq 0 ]; then
    echo "  [PASS] slice_raw_view_holds_nothing: views of uncounted memory hold nothing; views that may be counted hold their own"
fi
[ "$fails" -eq 0 ]
