#!/bin/sh
# #2458: a closure's write to a captured variable through `++` / `--`, a
# field (`p.x = v`, `p.x += v`) or an element (`arr[i] = v`) reaches the
# variable.
#
# Promotion to a shared cell (so the closure and its caller see one
# variable) is decided by which captures the body writes. Only a bare
# `name = ...` / `name op= ...` counted, so `h = || { n++ }` called twice
# left n at 0, and `p.x += 10` changed the closure's own copy: it printed 11
# twice and the caller still saw 1.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] closure_write_through_capture: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
import std.string

struct Inner { y: int }
struct Rec { name: string, count: int }
struct Named { name: string }
struct P { x: int, inner: Inner }
struct Box3 { vals: int[3] }

main() {
    n = 0
    h = || { n++ }
    call(h)
    call(h)
    println("inc ${n}")

    d = 10
    g = || { d-- }
    call(g)
    println("dec ${d}")

    p = P { x: 1, inner: Inner { y: 5 } }
    f = || {
        p.x += 10
        println("inside ${p.x}")
    }
    call(f)
    call(f)
    println("outside ${p.x}")

    set_x = || { p.x = 7 }
    call(set_x)
    deep = || { p.inner.y = p.inner.y * 3 }
    call(deep)
    println("field ${p.x} nested ${p.inner.y}")

    // A fixed-size array in a struct: the struct is promoted, so an element
    // write reaches the caller (the route #2474 suggests).
    b = Box3 { vals: [1, 2, 3] }
    w = || { b.vals[1] = 9 }
    call(w)
    println("element ${b.vals[1]}")

    // A struct owning a heap string: the field write and the owned-string
    // store both go through the shared cell (its release is leak-checked
    // by tests/regression/test_closure_struct_cell_owned_fields.ae).
    r = Rec { name: string.concat("a", "b"), count: 0 }
    bump = || { r.count += 1 }
    call(bump)
    call(bump)
    rename = || { r.name = string.concat(r.name, "!") }
    call(rename)
    r.name = string.concat(r.name, "+")
    println("rec ${r.name} ${r.count}")

    // A heap.new box is shared through its pointer: not promoted, the
    // write reaches the caller as it always did.
    nb = heap.new(Named)
    nb.name = string.concat("n", "1")
    g2 = || { nb.name = string.concat("n", "2") }
    call(g2)
    println("box ${nb.name}")
    heap.free(nb)
}
AE

# A write to a captured bare fixed-size array cannot reach the caller yet
# (#2474): it is refused at the source line, not compiled into a lost write.
# An element write, and a whole-array assignment, which in a closure body is
# a write to the captured variable too (it used to hide an element write in
# the same body from the check).
cat > "$tmp/arr.ae" <<'AE'
main() {
    int[3] arr = [1, 2, 3]
    w = || { arr[1] = 9 }
    call(w)
    println("${arr[1]}")
}
AE
cat > "$tmp/arr_whole.ae" <<'AE'
main() {
    int[3] arr = [1, 2, 3]
    f = || {
        if arr[1] > 100 { arr = [4, 5, 6] }
    }
    call(f)
    println("${arr[1]}")
}
AE
for prog in arr arr_whole; do
    out="$("$AE" run "$tmp/$prog.ae" 2>&1)"
    if ! printf '%s\n' "$out" | grep -q "a closure cannot write to 'arr', a fixed-size array it captures"; then
        echo "  [FAIL] closure_write_through_capture: a captured array write was not refused ($prog)"
        printf '%s\n' "$out" | head -5 | sed 's/^/        /'
        exit 1
    fi
done

want="inc 2
dec 9
inside 11
inside 21
outside 21
field 7 nested 15
element 9
rec ab!+ 2
box n2"
got="$("$AE" run "$tmp/main.ae" 2>&1 | tr -d '\r' | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] closure_write_through_capture: wrong values"
    printf 'got:\n%s\nwant:\n%s\n' "$got" "$want" | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] closure_write_through_capture: ++, --, field and element writes in a closure reach the captured variable"
