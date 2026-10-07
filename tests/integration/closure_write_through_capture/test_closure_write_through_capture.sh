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

# A write to a captured fixed-size array reaches the caller (#2474). The
# array lives in a shared cell, a pointer to the whole array, as any other
# variable a closure writes does: an element write, `++` / `op=`, and a
# whole-array assignment (which, before cells could hold an array, was
# refused at compile time rather than compiled into a write the caller
# never saw). int, float and string elements; the enclosing function
# writing the array too; nested closures; a closure made in a loop; the
# array handed to a function after the closure ran; a module constant
# table read beside it. String elements are owned by the cell, and that is
# leak-checked by tests/regression/test_closure_array_cell_strings.ae.
cat > "$tmp/arrays.ae" <<'AE'
const STEP[] = [10, 20, 30]

total(xs: int[]) -> int {
    s = 0
    for i in 0..xs.len {
        s = s + xs[i]
    }
    return s
}

main() {
    int[3] arr = [1, 2, 3]
    w = || { arr[1] = 9 }
    call(w)
    println("element ${arr[1]} len ${arr.len}")

    bump = || {
        arr[0] += STEP[0]
        arr[2]++
    }
    call(bump)
    call(bump)
    arr[1] = arr[1] * 2
    println("ops ${arr[0]} ${arr[1]} ${arr[2]} total ${total(arr)}")

    f = || {
        if arr[1] > 10 { arr = [4, 5] }
    }
    call(f)
    println("whole ${arr[0]} ${arr[1]} ${arr[2]}")

    swap = || { arr = [arr[1], arr[0], arr[2] + 1] }
    call(swap)
    println("swap ${arr[0]} ${arr[1]} ${arr[2]}")

    outer = || {
        inner = || { arr[2] = 100 }
        inner()
        arr[0] = arr[0] + 1
    }
    outer()
    println("nested ${arr[0]} ${arr[2]}")

    int[4] counts = [0, 0, 0, 0]
    i = 0
    while i < 6 {
        hit = || { counts[i % 4] += 1 }
        hit()
        i = i + 1
    }
    println("loop ${counts[0]} ${counts[1]} ${counts[2]} ${counts[3]}")

    float[2] fs = [1.5, 2.5]
    grow = || { fs[0] = fs[0] + fs[1] }
    call(grow)
    println("float ${fs[0]}")

    string[2] names = ["a", "b"]
    rename = || { names[1] = "${names[0]}${names[1]}!" }
    call(rename)
    call(rename)
    println("string ${names[0]} ${names[1]}")
}
AE
want_arrays="element 9 len 3
ops 21 18 5 total 44
whole 4 5 0
swap 5 4 1
nested 6 100
loop 2 2 1 1
float 4
string a aab!!"
got="$("$AE" run "$tmp/arrays.ae" 2>&1 | tr -d '\r' | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
if [ "$got" != "$want_arrays" ]; then
    echo "  [FAIL] closure_write_through_capture: a write to a captured array did not reach the caller"
    printf 'got:\n%s\nwant:\n%s\n' "$got" "$want_arrays" | sed 's/^/        /'
    exit 1
fi

# An actor's state array is the actor's, not a capture the closure could
# share in a cell: a closure in a handler that writes an element of one is
# refused, as a write to any other state field is. An array local to the
# handler is shared with its closure like any other.
cat > "$tmp/actor.ae" <<'AE'
message Push { v: int }
message Get {}

actor A {
    state int[4] hist
    receive {
        Push(v) -> {
            local = [0, 0]
            f = || {
                local[0] = v
                local[1] = local[1] + 1
            }
            f()
            f()
            hist[0] = hist[0] + local[0]
            hist[1] = hist[1] + local[1]
        }
        Get() -> { reply hist[0] * 100 + hist[1] }
    }
}

main() {
    a = spawn(A())
    a ! Push { v: 3 }
    a ! Push { v: 4 }
    n = a ? Get {}
    println("state ${n}")
}
AE
got="$("$AE" run "$tmp/actor.ae" 2>&1 | tr -d '\r' | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
if [ "$got" != "state 704" ]; then
    echo "  [FAIL] closure_write_through_capture: a handler-local array written by a closure"
    printf '%s\n' "$got" | head -5 | sed 's/^/        /'
    exit 1
fi
cat > "$tmp/state_write.ae" <<'AE'
message Push { v: int }

actor A {
    state int[4] hist
    receive {
        Push(v) -> {
            f = || { hist[1] = v }
            f()
        }
    }
}

main() {
    a = spawn(A())
    a ! Push { v: 3 }
}
AE
out="$("$AE" run "$tmp/state_write.ae" 2>&1)"
if ! printf '%s\n' "$out" | grep -q "closure inside actor 'A' handler writes state field 'hist'"; then
    echo "  [FAIL] closure_write_through_capture: a closure writing an actor's state array was not refused"
    printf '%s\n' "$out" | head -5 | sed 's/^/        /'
    exit 1
fi

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
echo "  [PASS] closure_write_through_capture: ++, --, field, element and fixed-size array writes in a closure reach the captured variable"
