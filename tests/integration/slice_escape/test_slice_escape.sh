#!/bin/sh
# A slice of a function's own storage must not outlive the function (E0900).
#
# An array literal in a slice slot and a function's own fixed-size array
# live in its frame. A slice of either that left the function viewed memory
# the next call reused: `gs = [11, 22, 33]` in `fill()`, then a call that
# fills its own frame, printed `first 0` for `gs[0]`. A module-level `T[]`
# could not hold even the empty slice before #2785, so this came with it;
# a returned slice of a local array dangled before that too. Each way a view
# can leave the frame is refused at the value that leaves: a return, a store
# to a module-level `var`, a store to a field of something the function did
# not make for itself, and a closure that captured such a slice. A slice used
# while the function runs, passed down, or of a slice parameter or a `make`
# buffer, compiles and runs as before. A fixed-size array parameter is the
# function's own copy, so a slice of it is refused too. What a name holds
# where it leaves decides: one re-bound to a `make` buffer before it leaves
# is no view any more, when it is on every path to the exit.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] slice_escape: ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fails=0

# refused <name> <line:col> <text>: `ae check` refuses <name>.ae with E0900
# at that position, its message containing <text>.
refused() {
    name="$1"; at="$2"; text="$3"
    out=$(cd "$TMP" && "$AE" check "$name.ae" 2>&1)
    if [ $? -eq 0 ]; then
        echo "  [FAIL] $name: accepted"
        echo "$out" | sed 's/^/        /'
        fails=$((fails + 1)); return
    fi
    case "$out" in
        *"error[E0900]"*"$text"*"$name.ae:$at"*) ;;
        *) echo "  [FAIL] $name: not E0900 \"$text\" at $at:"
           echo "$out" | sed 's/^/        /'
           fails=$((fails + 1)) ;;
    esac
}

cat > "$TMP/global_literal.ae" <<'AE'
var gs: int[] = null

fill() {
    gs = [11, 22, 33]
}

main() {
    fill()
    println("${gs.len}")
}
AE
refused global_literal 4:10 "the module-level \`gs\` is given a slice of the storage of 'fill' (an array literal)"

cat > "$TMP/global_array.ae" <<'AE'
var gs: int[] = null

fill() {
    arr = [5, 6, 7]
    gs = arr
}

main() {
    fill()
    println("${gs.len}")
}
AE
refused global_array 5:10 "the module-level \`gs\` is given a slice of the storage of 'fill' (the array \`arr\`)"

cat > "$TMP/return_literal.ae" <<'AE'
lit() -> int[] {
    return [1, 2, 3]
}

main() {
    println("${lit().len}")
}
AE
refused return_literal 2:12 "'lit' returns a slice of its own storage (an array literal)"

cat > "$TMP/return_local.ae" <<'AE'
loc() -> int[] {
    let a: int[3] = [4, 5, 6]
    s = a[0..2]
    return s
}

main() {
    println("${loc().len}")
}
AE
refused return_local 4:12 "'loc' returns a slice of its own storage (\`s\`, which holds a slice of it)"

cat > "$TMP/field.ae" <<'AE'
extern malloc(n: long) -> ptr

struct Bag {
    xs: int[]
}

fill(b: *Bag) {
    let a: int[3] = [7, 8, 9]
    b.xs = a
}

main() {
    b = malloc(sizeof(Bag)) as *Bag
    fill(b)
    println("${b.xs.len}")
}
AE
refused field 9:12 "a field of \`b\`, which outlives 'fill', is given a slice of its own storage (the array \`a\`)"

cat > "$TMP/struct_value.ae" <<'AE'
struct Bag {
    xs: int[]
}

wrap() -> Bag {
    let a: int[3] = [7, 8, 9]
    b = Bag { xs: a }
    return b
}

main() {
    println("${wrap().xs.len}")
}
AE
refused struct_value 8:12 "'wrap' returns a slice of its own storage (\`b\`, which holds a slice of it)"

cat > "$TMP/capture.ae" <<'AE'
counter() -> fn {
    let a: int[3] = [7, 8, 9]
    s = a[0..3]
    return || { return s.len }
}

main() {
    println("${call(counter())}")
}
AE
refused capture 4:12 "'counter' returns a slice of its own storage (a closure that captured a slice of it)"

cat > "$TMP/array_param.ae" <<'AE'
first_two(a: int[3]) -> int[] {
    return a[0..2]
}

main() {
    s = first_two([11, 22, 33])
    println("${s[0]} ${s[1]}")
}
AE
refused array_param 2:13 "'first_two' returns a slice of its own storage (the array parameter \`a\`, its own copy)"

cat > "$TMP/branch_keeps.ae" <<'AE'
pick(n: int, c: bool) -> int[] {
    arr = [1, 2, 3]
    let v: int[] = arr
    if c {
        v = make([]int, n)
    }
    return v
}

main() {
    println("${pick(2, true).len}")
}
AE
refused branch_keeps 7:12 "'pick' returns a slice of its own storage (\`v\`, which holds a slice of it)"

cat > "$TMP/loop_keeps.ae" <<'AE'
pick(n: int) -> int[] {
    arr = [1, 2, 3]
    let v: int[] = make([]int, n)
    i = 0
    while i < n {
        if i == 1 {
            break
        }
        v = arr
        i = i + 1
    }
    return v
}

main() {
    println("${pick(3).len}")
}
AE
refused loop_keeps 12:12 "'pick' returns a slice of its own storage (\`v\`, which holds a slice of it)"

# What stays in the frame, or views storage that outlives it, runs.
cat > "$TMP/stays.ae" <<'AE'
struct Bag {
    xs: int[]
}

total(xs: int[]) -> int {
    s = 0
    for i in 0..xs.len {
        s = s + xs[i]
    }
    return s
}

made(n: int) -> int[] {
    return make([]int, n)
}

local_use() -> int {
    let a: int[3] = [4, 5, 6]
    s = a[0..2]
    b = Bag { xs: a }
    k = || { return s.len }
    return total(a) + s.len + b.xs.len + call(k) + total([1, 2])
}

main() {
    m = made(4)
    println("stays ${m.len} ${local_use()}")
    free(m)
}
AE
out=$(cd "$TMP" && AETHER_HOME="$ROOT" "$AE" run stays.ae 2>&1)
case "$out" in
    *"stays 4 25"*) ;;
    *) echo "  [FAIL] stays: not \"stays 4 25\":"
       echo "$out" | sed 's/^/        /'
       fails=$((fails + 1)) ;;
esac

# A name re-bound to storage that outlives the frame before it leaves holds
# no view of the frame any more: what it holds where it leaves decides.
cat > "$TMP/rebound.ae" <<'AE'
struct Holder {
    data: int[]
    n: int
}

fill(n: int) -> int[] {
    arr = [1, 2, 3]
    let v: int[] = arr
    t = v[0] + v[1]
    v = make([]int, n)
    v[0] = t
    return v
}

keep(n: int) -> Holder {
    arr = [1, 2, 3]
    h = Holder { data: arr, n: 0 }
    h.n = h.data[1]
    h.data = make([]int, n)
    h.data[0] = h.n
    return h
}

both(n: int, c: bool) -> int[] {
    arr = [5, 6, 7]
    let v: int[] = arr
    k = v[2]
    if c {
        v = make([]int, n)
    } else {
        v = make([]int, n + 1)
    }
    v[0] = k
    return v
}

main() {
    s = fill(3)
    h = keep(4)
    b = both(2, false)
    println("rebound ${s[0]} ${s.len} ${h.data[0]} ${h.data.len} ${b[0]} ${b.len}")
    free(s)
    free(h.data)
    free(b)
}
AE
out=$(cd "$TMP" && AETHER_HOME="$ROOT" "$AE" run rebound.ae 2>&1)
case "$out" in
    *"rebound 3 3 2 4 7 3"*) ;;
    *) echo "  [FAIL] rebound: not \"rebound 3 3 2 4 7 3\":"
       echo "$out" | sed 's/^/        /'
       fails=$((fails + 1)) ;;
esac

# A re-bind made by a closure call is not counted: a name that holds a view
# before `reset()` still holds one after it, whatever the closure does. That
# refuses a closure that does re-bind the name to a `make` buffer (a rare
# false positive), so that no closure that re-binds it back to a view, or
# may not re-bind it at all, is accepted. Following a closure call soundly
# is future work (#2837).
cat > "$TMP/closure_rebind.ae" <<'AE'
pick() -> int[] {
    arr = [1, 2, 3]
    let v: int[] = arr
    reset = || {
        v = make([]int, 4)
    }
    reset()
    return v
}

main() {
    println("${pick().len}")
}
AE
refused closure_rebind 8:12 "'pick' returns a slice of its own storage (\`v\`, which holds a slice of it)"

cat > "$TMP/closure_rebind_field.ae" <<'AE'
struct Holder {
    data: int[]
}

pick() -> Holder {
    arr = [1, 2, 3]
    h = Holder { data: arr }
    reset = || {
        h.data = make([]int, 6)
    }
    reset()
    return h
}

main() {
    println("${pick().data.len}")
}
AE
refused closure_rebind_field 12:12 "'pick' returns a slice of its own storage (\`h\`, which holds a slice of it)"

cat > "$TMP/closure_rebind_back.ae" <<'AE'
pick() -> int[] {
    arr = [1, 2, 3]
    let v: int[] = arr
    reset = || {
        v = make([]int, 4)
        v = arr
    }
    reset()
    return v
}

main() {
    println("${pick().len}")
}
AE
refused closure_rebind_back 9:12 "'pick' returns a slice of its own storage (\`v\`, which holds a slice of it)"

cat > "$TMP/closure_if.ae" <<'AE'
pick(n: int) -> int[] {
    arr = [1, 2, 3]
    let v: int[] = arr
    reset = || {
        if n > 0 {
            v = make([]int, 4)
        }
    }
    reset()
    return v
}

main() {
    println("${pick(1).len}")
}
AE
refused closure_if 10:12 "'pick' returns a slice of its own storage (\`v\`, which holds a slice of it)"

cat > "$TMP/call_maybe.ae" <<'AE'
pick(n: int) -> int[] {
    arr = [1, 2, 3]
    let v: int[] = arr
    reset = || {
        v = make([]int, 4)
    }
    if n > 0 {
        reset()
    }
    return v
}

main() {
    println("${pick(1).len}")
}
AE
refused call_maybe 10:12 "'pick' returns a slice of its own storage (\`v\`, which holds a slice of it)"

# A closure that may store a slice of the frame's storage into one of its
# names gives that name a view from where the closure is written on,
# whenever it is called, or whether. A store a closure makes through a
# pointer is judged as the same store written in the function is.
cat > "$TMP/closure_stores.ae" <<'AE'
pick() -> int[] {
    arr = [1, 2, 3]
    let v: int[] = make([]int, 4)
    setview = || {
        v = arr
    }
    setview()
    return v
}

main() {
    println("${pick().len}")
}
AE
refused closure_stores 8:12 "'pick' returns a slice of its own storage (\`v\`, which holds a slice of it)"

cat > "$TMP/closure_never_called.ae" <<'AE'
pick() -> int[] {
    arr = [1, 2, 3]
    let v: int[] = make([]int, 4)
    setview = || {
        v = arr
    }
    return v
}

main() {
    println("${pick().len}")
}
AE
refused closure_never_called 7:12 "'pick' returns a slice of its own storage (\`v\`, which holds a slice of it)"

cat > "$TMP/closure_ptr_store.ae" <<'AE'
struct Holder {
    data: int[]
}

pick() -> Holder {
    arr = [1, 2, 3]
    h = Holder { data: make([]int, 2) }
    hp = &h
    setv = || {
        hp.data = arr
    }
    setv()
    return h
}

main() {
    println("${pick().data.len}")
}
AE
refused closure_ptr_store 10:19 "a field of \`hp\`, which may outlive 'pick', is given by this closure a slice of the storage of 'pick' (the array \`arr\`)"

cat > "$TMP/closure_safe.ae" <<'AE'
fresh() -> int[] {
    let v: int[] = make([]int, 4)
    reset = || {
        v = make([]int, 5)
    }
    reset()
    return v
}

summed() -> int[] {
    arr = [1, 2, 3]
    let v: int[] = make([]int, 4)
    total = 0
    add = || {
        total = total + arr[0] + arr[2]
    }
    add()
    v[0] = total
    return v
}

main() {
    a = fresh()
    b = summed()
    println("closure safe ${a.len} ${b[0]}")
    free(a)
    free(b)
}
AE
out=$(cd "$TMP" && AETHER_HOME="$ROOT" "$AE" run closure_safe.ae 2>&1)
case "$out" in
    *"closure safe 5 4"*) ;;
    *) echo "  [FAIL] closure_safe: not \"closure safe 5 4\":"
       echo "$out" | sed 's/^/        /'
       fails=$((fails + 1)) ;;
esac

# Only a view of the frame's own storage taints: a closure storing a slice
# parameter, a slice of one, a field given one, a closure parameter the
# closure is called with a non-view for, a slice of a `make` buffer, a local
# holding one, a module-level `var` or `[]` leaves the name as it was.
cat > "$TMP/closure_not_views.ae" <<'AE'
struct Holder {
    data: int[]
}

var gs: int[] = null

from_param(xs: int[]) -> int[] {
    let v: int[] = make([]int, 2)
    set = || {
        v = xs
    }
    set()
    return v
}

from_param_slice(xs: int[]) -> int[] {
    let v: int[] = make([]int, 2)
    set = || {
        v = xs[0..3]
    }
    set()
    return v
}

field_from_param(xs: int[]) -> Holder {
    h = Holder { data: make([]int, 1) }
    set = || {
        h.data = xs
    }
    set()
    return h
}

closure_param(xs: int[]) -> int[] {
    let v: int[] = make([]int, 2)
    set = |s: int[]| {
        v = s
    }
    set(xs)
    return v
}

from_buffer() -> int[] {
    buf = make([]int, 8)
    let v: int[] = make([]int, 2)
    set = || {
        v = buf[0..4]
    }
    set()
    return v
}

from_local() -> int[] {
    a = make([]int, 5)
    let v: int[] = make([]int, 2)
    set = || {
        v = a
    }
    set()
    return v
}

from_global() -> int[] {
    let v: int[] = make([]int, 2)
    set = || {
        v = gs
    }
    set()
    return v
}

to_empty() -> int[] {
    let v: int[] = make([]int, 2)
    set = || {
        v = []
    }
    set()
    return v
}

main() {
    xs = make([]int, 6)
    gs = make([]int, 7)
    println("safe ${from_param(xs).len} ${from_param_slice(xs).len} ${field_from_param(xs).data.len} ${closure_param(xs).len} ${from_buffer().len} ${from_local().len} ${from_global().len} ${to_empty().len}")
}
AE
out=$(cd "$TMP" && AETHER_HOME="$ROOT" "$AE" run closure_not_views.ae 2>&1)
case "$out" in
    *"safe 6 3 6 6 4 5 7 0"*) ;;
    *) echo "  [FAIL] closure_not_views: not \"safe 6 3 6 6 4 5 7 0\":"
       echo "$out" | sed 's/^/        /'
       fails=$((fails + 1)) ;;
esac

cat > "$TMP/closure_local_view.ae" <<'AE'
pick() -> int[] {
    arr = [1, 2, 3]
    let v: int[] = make([]int, 2)
    set = || {
        let w: int[] = arr
        v = w
    }
    set()
    return v
}

main() {
    println("${pick().len}")
}
AE
refused closure_local_view 9:12 "'pick' returns a slice of its own storage (\`v\`, which holds a slice of it)"

cat > "$TMP/closure_param_view.ae" <<'AE'
pick() -> int[] {
    arr = [1, 2, 3]
    let v: int[] = make([]int, 2)
    set = |s: int[]| {
        v = s
    }
    set(arr)
    return v
}

main() {
    println("${pick().len}")
}
AE
refused closure_param_view 8:12 "'pick' returns a slice of its own storage (\`v\`, which holds a slice of it)"

cat > "$TMP/closure_nested_store.ae" <<'AE'
pick() -> int[] {
    arr = [1, 2, 3]
    let v: int[] = make([]int, 2)
    outer = || {
        inner = || {
            v = arr
        }
        inner()
    }
    outer()
    return v
}

main() {
    println("${pick().len}")
}
AE
refused closure_nested_store 11:12 "'pick' returns a slice of its own storage (\`v\`, which holds a slice of it)"

cat > "$TMP/closure_in_loop.ae" <<'AE'
pick(n: int) -> int[] {
    arr = [1, 2, 3]
    let v: int[] = make([]int, 2)
    i = 0
    while i < n {
        set = || {
            v = arr
        }
        set()
        i = i + 1
    }
    return v
}

main() {
    println("${pick(1).len}")
}
AE
refused closure_in_loop 12:12 "'pick' returns a slice of its own storage (\`v\`, which holds a slice of it)"

# #2846: a closure's own frame ends when it returns. Its own array literal,
# stored into a variable of the frame it is written in (directly or through
# a local of its own), would view the closure's freed stack.
cat > "$TMP/closure_own_literal.ae" <<'AE'
main() {
    let v: int[] = make([]int, 3)
    set = || {
        v = [7, 8, 9]
    }
    set()
    println("${v[0]}")
}
AE
refused closure_own_literal 4:13 "\`v\`, a variable of 'main', is given by this closure a slice of its own storage (an array literal)"

cat > "$TMP/closure_own_literal_local.ae" <<'AE'
f() -> int[] {
    let v: int[] = make([]int, 2)
    st = || {
        t = [4, 5, 6]
        v = t
    }
    st()
    return v
}

main() {
    println("${f().len}")
}
AE
refused closure_own_literal_local 5:13 "\`v\`, a variable of 'f', is given by this closure a slice of its own storage (the array \`t\`)"

# #2846: a bare binding inside a closure of a name the frame does not have is
# the closure's own local, and carries a view of the frame's storage as a
# written-type one does.
cat > "$TMP/closure_inferred_local.ae" <<'AE'
f(n: int) -> int[] {
    arr = [1, 2, 3]
    let w: int[] = arr
    let v: int[] = make([]int, 2)
    st = || {
        t = w
        v = t
    }
    st()
    return v
}

main() {
    println("${f(1).len}")
}
AE
refused closure_inferred_local 10:12 "'f' returns a slice of its own storage (\`v\`, which holds a slice of it)"

# #2846: a closure the frame binds, returning a view of the frame's storage:
# its call's result is that view.
cat > "$TMP/closure_call_result.ae" <<'AE'
pick() -> int[] {
    arr = [1, 2, 3]
    get = || {
        let w: int[] = arr
        return w
    }
    return call(get)
}

main() {
    println("${pick().len}")
}
AE
refused closure_call_result 7:12 "'pick' returns a slice of its own storage (what \`get\` returns, a slice of it)"

# The closure's own slices used inside it, a buffer the closure makes stored
# out, and a closure returning a `make` buffer stay accepted.
cat > "$TMP/closure_own_ok.ae" <<'AE'
fill() -> int[] {
    let v: int[] = make([]int, 2)
    st = || {
        t = [4, 5, 6]
        total = t[0] + t[2]
        v = make([]int, total)
    }
    st()
    return v
}

fresh() -> int[] {
    get = || {
        return make([]int, 3)
    }
    return call(get)
}

main() {
    println("own ok ${fill().len} ${fresh().len}")
}
AE
out=$(cd "$TMP" && AETHER_HOME="$ROOT" "$AE" run closure_own_ok.ae 2>&1)
case "$out" in
    *"own ok 10 3"*) ;;
    *) echo "  [FAIL] closure_own_ok: not \"own ok 10 3\":"
       echo "$out" | sed 's/^/        /'
       fails=$((fails + 1)) ;;
esac

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] slice_escape: a slice of a function's own storage stays in it"
