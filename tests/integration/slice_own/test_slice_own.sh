#!/bin/sh
# Slice ownership, what the checker refuses: a slice local used after its
# buffer moved out of it (bound to another name, freed, consumed by
# `append`, given to a `@keep` parameter), on every path or some (E0906); a
# parameter freed, appended to or pushed to, or given to a `@keep`
# parameter, which would release or move the caller's buffer (E0901);
# `push` on what is no place. And what it takes: a moved name bound again,
# a loop that re-binds what it moves, a view taken with `xs[..]` before a
# move, `@keep` parameters, `defer free`, a view that leaves the function
# (its buffer is then kept, as before ownership). And that the
# heap stays flat over rounds that release what they make, where a leak of
# one buffer a round is seen. What else it does at run time is
# tests/regression/test_slice_own.ae's.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] slice_own: ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fails=0

# refused <name> <code> <text> <line:col>: `ae check` refuses the program on
# stdin with error <code> whose message holds <text>, at that position.
refused() {
    cat > "$TMP/$1.ae"
    out=$(cd "$TMP" && AETHER_HOME="$ROOT" "$AE" check "$1.ae" 2>&1)
    if [ $? -eq 0 ]; then
        echo "  [FAIL] $1: accepted"
        fails=$((fails + 1))
        return
    fi
    case "$out" in
        *"error[$2]"*"$3"*"$1.ae:$4"*) ;;
        *) echo "  [FAIL] $1: not $2 \"$3\" at $4"
           echo "$out" | sed 's/^/        /'
           fails=$((fails + 1)) ;;
    esac
}

# accepted <name>: `ae check` takes the program on stdin.
accepted() {
    cat > "$TMP/$1.ae"
    out=$(cd "$TMP" && AETHER_HOME="$ROOT" "$AE" check "$1.ae" 2>&1)
    if [ $? -ne 0 ]; then
        echo "  [FAIL] $1: refused"
        echo "$out" | sed 's/^/        /' | head -8
        fails=$((fails + 1))
    fi
}

refused use_after_move E0906 "'xs' is used after it was moved (line 3): an owned slice has one owner" 4:16 <<'AE'
main() {
    xs = make([]int, 3)
    ys = xs
    println("${xs.len} ${ys.len}")
}
AE

refused use_after_free E0906 "'xs' is used after it was freed (line 3)" 4:16 <<'AE'
main() {
    xs = make([]int, 3)
    free(xs)
    println("${xs.len}")
}
AE

refused use_after_append E0906 "'xs' is used after \`append\` consumed it (line 3)" 4:16 <<'AE'
main() {
    xs = make([]int, 3)
    ys = append(xs, 4)
    println("${xs.len} ${ys.len}")
}
AE

refused use_after_keep E0906 "'xs' is used after it was given to 'take_all', which keeps it (line 7)" 8:16 <<'AE'
take_all(v: @keep int[]) -> int {
    return v.len
}

main() {
    xs = make([]int, 3)
    n = take_all(xs)
    println("${xs.len} ${n}")
}
AE

refused maybe_moved E0906 "'xs' is used after a path where it was moved (line 5)" 7:16 <<'AE'
main() {
    xs = make([]int, 3)
    ys = make([]int, 0)
    if xs.len > 2 {
        ys = xs
    }
    println("${xs.len} ${ys.len}")
}
AE

refused moved_in_loop E0906 "'xs' is used after a path where it was moved (line 5)" 5:14 <<'AE'
main() {
    xs = make([]int, 3)
    ys = make([]int, 0)
    for i in 0..3 {
        ys = xs
    }
    println("${ys.len}")
}
AE

refused free_param E0901 "cannot free 'xs': it is a parameter, which borrows the caller's slice" 2:10 <<'AE'
drop_it(xs: int[]) {
    free(xs)
}

main() {
    a = make([]int, 2)
    drop_it(a)
}
AE

# The same under a program's own `extern free(p: ptr)`: free of a slice is the
# builtin release there too (#3450), so the parameter is still borrowed.
refused free_param_extern_free E0901 "cannot free 'xs': it is a parameter, which borrows the caller's slice" 4:10 <<'AE'
extern free(p: ptr)

drop_it(xs: int[]) {
    free(xs)
}

main() {
    a = make([]int, 2)
    drop_it(a)
}
AE

refused push_param E0901 "cannot push to 'xs': it is a parameter" 2:7 <<'AE'
add_one(xs: int[]) {
    xs.push(1)
}

main() {
    a = make([]int, 2)
    add_one(a)
}
AE

refused append_param E0901 "cannot append to 'xs': it is a parameter" 2:19 <<'AE'
grow(xs: int[]) -> int[] {
    return append(xs, 1)
}

main() {
    a = grow(make([]int, 2))
    println("${a.len}")
}
AE

refused param_to_keep E0901 "cannot give 'xs' to 'take_all', which keeps it (\`@keep\`)" 6:21 <<'AE'
take_all(v: @keep int[]) -> int {
    return v.len
}

pass_on(xs: int[]) -> int {
    return take_all(xs)
}

main() {
    a = make([]int, 2)
    println("${pass_on(a)}")
}
AE

# A view of a buffer that leaves the function keeps the buffer: it is the
# program's again, as before ownership, and is not released.
accepted view_returned <<'AE'
tail(n: int) -> int[] {
    xs = make([]int, n)
    return xs[1..]
}

main() {
    t = tail(3)
    println("${t.len}")
}
AE

refused push_not_place E0200 "\`push\` grows a slice in place" 6:9 <<'AE'
mk() -> int[] {
    return make([]int, 1)
}

main() {
    mk().push(2)
}
AE

accepted rebound_after_move <<'AE'
main() {
    xs = make([]int, 3)
    ys = xs
    xs = make([]int, 1)
    println("${xs.len} ${ys.len}")
}
AE

accepted loop_rebinds <<'AE'
main() {
    int[] xs = []
    for i in 0..5 {
        xs = append(xs, i)
        xs.push(i)
    }
    acc = make([]int, 0)
    for i in 0..3 {
        acc = append(acc, xs[i])
    }
    println("${xs.len} ${acc.len}")
}
AE

accepted borrow_before_move <<'AE'
main() {
    xs = make([]int, 3)
    v = xs[..]
    println("${v.len} ${xs.len}")
    ys = xs
    println("${ys.len}")
}
AE

accepted keep_params <<'AE'
take_all(v: @keep int[]) -> int {
    return v.len
}

grow(xs: @keep int[]) -> int[] {
    xs.push(1)
    return xs
}

pass_on(xs: @keep int[]) -> int {
    return take_all(xs)
}

main() {
    a = grow(make([]int, 2))
    a = grow(a)
    println("${take_all(a)} ${pass_on(make([]int, 4))}")
}
AE

accepted old_frees <<'AE'
main() {
    xs = make([]int, 3)
    defer free(xs)
    ys = make([]string, 2)
    ys[0] = "a"
    free(ys)
    println("${xs.len}")
}
AE

# Views that keep a buffer for good, as before ownership (they leak by
# design, so the leak gate's regression tests do not hold them): a view a
# function hands back kept in a local of an outer scope, and a view held
# while its owner is bound again. Released, each read back the filler.
cat > "$TMP/kept.ae" <<'AE'
around(k: int) -> int[] {
    out = make([]int, 4)
    for i in 0..4 {
        out[i] = k * 10 + i
    }
    return out
}

churn() -> int {
    t = 0
    for r in 0..64 {
        b = make([]int, 4)
        for i in 0..4 {
            b[i] = -7
        }
        t = t + b[0]
    }
    return t
}

first_two(xs: int[]) -> int[] {
    return xs[0..2]
}

kept_view(n: int) -> int {
    int[] last = []
    for k in 0..n {
        xs = around(k)
        last = first_two(xs)
    }
    churn()
    return last[0] + last[1]
}

rebound_view() -> int {
    xs = around(5)
    v = xs[1..3]
    xs = around(6)
    churn()
    return v[0] + v[1] + xs[0]
}

main() {
    println("kept ${kept_view(5)} ${rebound_view()}")
}
AE
out=$(cd "$TMP" && AETHER_HOME="$ROOT" "$AE" run kept.ae 2>&1)
case "$out" in
    *"kept 81 163"*) ;;
    *) echo "  [FAIL] kept: not \"kept 81 163\":"
       echo "$out" | sed 's/^/        /'
       fails=$((fails + 1)) ;;
esac

# The heap does not grow: 200 rounds of buffers released at scope exit (a
# make buffer, one pushed and moved, rows of a slice of slices replaced,
# buffers of strings in a loop left by `break`), 256 KB each, against the
# same rounds leaking one more buffer into a field nothing frees, which the
# check must catch (about 50 MB). A leak tool's job too (the leak gate runs
# tests/regression/test_slice_own.ae); this one needs none.
cat > "$TMP/grow.ae" <<'AE'
import std.mem
import std.os

struct Bag {
    items: int[]
}

fill(n: int) -> int[] {
    int[] xs = []
    for i in 0..n {
        xs.push(i)
    }
    return xs
}

// One round: every way a buffer is given up, each 64K ints (256 KB).
round(leak: bool) -> int {
    n = 65536
    a = make([]int, n)
    b = fill(n)
    c = b
    rows = make([]int[], 2)
    rows[0] = fill(n)
    rows[1] = fill(n)
    rows[1] = fill(n)
    for i in 0..3 {
        t = make([]string, 4)
        t[0] = "s${i}"
        if i == 1 {
            break
        }
    }
    total = a.len + c.len + rows[1].len
    if leak {
        bag = Bag { items: fill(n) }   // a field: freed by hand, and here it is not
        total = total + bag.items.len
    }
    return total
}

main() {
    leak = os.args_count() > 1
    for i in 0..20 {
        round(leak)
    }
    h0 = mem.heap_in_use()
    for i in 0..200 {
        round(leak)
    }
    grew = (mem.heap_in_use() - h0) / 1048576
    println("grew ${grew} MB")
}
AE
if (cd "$TMP" && AETHER_HOME="$ROOT" "$AE" build grow.ae -o grow >/dev/null 2>&1); then
    owned=$("$TMP/grow" | sed -n 's/^grew \(-*[0-9]*\) MB$/\1/p')
    # The control leaks on purpose. Under ASan (the sweep's AETHER_SANITIZER
    # =asan run) LeakSanitizer is told so for that one run, whose leak is the
    # point; the run that releases what it makes is still checked by it.
    if [ "${AETHER_SANITIZER:-}" = asan ]; then
        leaked=$(ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0" "$TMP/grow" leak \
            | sed -n 's/^grew \(-*[0-9]*\) MB$/\1/p')
    else
        leaked=$("$TMP/grow" leak | sed -n 's/^grew \(-*[0-9]*\) MB$/\1/p')
    fi
    if [ -z "$owned" ] || [ "$owned" -gt 8 ]; then
        echo "  [FAIL] grow: the heap grew by ${owned:-?} MB over 200 rounds that release what they make"
        fails=$((fails + 1))
    fi
    if [ -z "$leaked" ] || [ "$leaked" -lt 25 ]; then
        echo "  [FAIL] grow: a leaked buffer a round was not seen (${leaked:-?} MB)"
        fails=$((fails + 1))
    fi
else
    echo "  [FAIL] grow: did not build"
    fails=$((fails + 1))
fi

if [ "$fails" -ne 0 ]; then exit 1; fi
echo "  [PASS] slice_own: moved, freed and borrowed slices refused where used wrongly; owners compile"
