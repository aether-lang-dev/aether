#!/bin/sh
# An owned string argument to a closure called through an `fn` parameter is
# freed after the call (#2499).
#
# `apply(f: fn, s: string) { call(f, s) }` has no closure body to read, so
# passing `s` on was counted as an escape and `apply(g, mk("x"))` leaked the
# string, as did `call(f, mk(a))` inside such a function. Closures borrow
# their arguments: one that keeps a `string` parameter (a list, map or field
# store, a captured variable) keeps it through a store that takes a
# reference of its own (R3), so the caller frees its own after the call
# either way, and the kept value stays good. A closure's `ptr` parameter is
# always borrowed (E0902 refuses keeping one, tests/integration/
# keeps_borrowed). This checks the emitted C, that every program prints
# what it stored, and that a parameter returned in a struct literal is the
# struct's own reference, the caller's argument still freed (#2584).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] closure_args_borrowed: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

fail() {
    echo "  [FAIL] closure_args_borrowed: $1"
    [ -n "$2" ] && [ -f "$2" ] && sed 's/^/        /' "$2" | head -20
    exit 1
}

# run <name> <want>: the program prints `want`.
run() {
    "$AE" run "$tmp/$1.ae" >"$tmp/$1.log" 2>&1 || fail "$1 did not run" "$tmp/$1.log"
    got="$(tr -d '\r' < "$tmp/$1.log")"
    [ "$got" = "$2" ] || fail "$1: wrong output (want: $2)" "$tmp/$1.log"
    "$AETHERC" "$tmp/$1.ae" "$tmp/$1.c" >"$tmp/$1.gen" 2>&1 || fail "aetherc failed on $1" "$tmp/$1.gen"
}

# ---- no closure keeps a parameter: every owned argument is freed ----------
cat > "$tmp/borrowed.ae" <<'AE'
import std.string

mk(a: string) -> string {
    return string.concat(a, "!")
}

apply(f: fn, s: string) {
    call(f, s)
}

apply_owned(f: fn, a: string) {
    call(f, mk(a))
    f(mk(a))
}

echo(f: fn, a: string) -> string {
    string r = call(f, mk(a))
    return r
}

main() {
    count = 0
    look = | s: string | { count = count + string.length(s) }
    apply(look, mk("x"))
    apply_owned(look, "ab")
    same = | s: string | -> s
    held = | s: string | {
        show = || { println("held ${s}") }
        show()
        grow = || { s = string.concat(s, "+") }
        grow()
        println(s)
    }
    apply_owned(held, "h")
    println("${count} ${echo(same, "e")}")
    // A function used as a closure value that returns its parameter: its
    // adapter returns a copy, so the argument is still the caller's.
    println(echo(ident, "i"))
}

ident(s: string) -> string {
    return s
}
AE
run borrowed "held h!
h!+
held h!
h!+
8 e!
i!"
grep -q '_ad_[0-9]* = (char\*)(mk(((const char\*)&_ae_slit_[0-9]*))), aether_unwind_track_str_place((const void\*)\&_ad_[0-9]*, NULL), apply(look, _ad_[0-9]*), aether_unwind_forget((const void\*)\&_ad_[0-9]*), aether_heap_str_free' "$tmp/borrowed.c" \
    || fail "the argument to apply() is not freed" "$tmp/borrowed.c"
n="$(grep -c '_ad_[0-9]* = (char\*)(mk(a)), .*_aether_callee_fn(f\.fn, "empty_closure_call: .*"))(f\.env, _ad_[0-9]*), aether_unwind_forget((const void\*)\&_ad_[0-9]*), aether_heap_str_free' "$tmp/borrowed.c")"
[ "$n" = "3" ] || fail "an argument to a call through an fn parameter is not freed ($n of 3)" "$tmp/borrowed.c"

# ---- a closure keeps its `string` parameter: the store takes its own
# reference, so the caller still frees its argument and the kept value
# stays good.
for shape in list map field cell branch; do
    case $shape in
        list)     keeper='kept = list.new()
    keep = | s: string | { list.add(kept, s) }'
                  reader='v, _ = list.get_string(kept, 0)
    println(v)'
                  freer='list.free(kept)' ;;
        map)      keeper='kept = map.new()
    keep = | s: string | { map.put(kept, "k", s) }'
                  reader='v, _ = map.get_string(kept, "k")
    println(v)'
                  freer='map.free(kept)' ;;
        field)    keeper='hb = heap.new(Holder)
    keep = | s: string | { hb.name = s }'
                  reader='println(hb.name)'
                  freer='heap.free(hb)' ;;
        cell)     keeper='last = ""
    keep = | s: string | { last = s }'
                  reader='println(last)'
                  freer='' ;;
        branch)   keeper='kept = list.new()
    keep = | s: string | {
        if true {
            list.add(kept, s)
        }
    }'
                  reader='v, _ = list.get_string(kept, 0)
    println(v)'
                  freer='list.free(kept)' ;;
    esac
    cat > "$tmp/kept_$shape.ae" <<AE
import std.string
import std.list
import std.map

struct Holder {
    name: string
}

mk(a: string) -> string {
    return string.concat(a, "!")
}

apply(f: fn, s: string) {
    call(f, s)
}

main() {
    $keeper
    apply(keep, mk("k"))
    junk = mk("zzzzzzzzzzzzzzzz")
    $reader
    println(junk)
    $freer
}
AE
    run "kept_$shape" "k!
zzzzzzzzzzzzzzzz!"
    grep -q 'mk(((const char\*)&_ae_slit_[0-9]*))), aether_unwind_track_str_place((const void\*)\&_ad_[0-9]*, NULL), apply(keep, _ad_[0-9]*), aether_unwind_forget((const void\*)\&_ad_[0-9]*), aether_heap_str_free' "$tmp/kept_$shape.c" \
        || fail "kept_$shape: the closure's store takes its own reference, yet the argument is not freed" "$tmp/kept_$shape.c"
done

# ---- a struct literal returned with the parameter in it keeps it -----------
cat > "$tmp/struct_field.ae" <<'AE'
import std.string

struct Rec {
    name: string
}

mk(a: string) -> string {
    return string.concat(a, "!")
}

mkrec(s: string) -> Rec {
    return Rec { name: s }
}

main() {
    r = mkrec(mk("abc"))
    junk = mk("zzzzzzzzzzzzzzzz")
    println("${r.name} ${junk}")
}
AE
run struct_field "abc! zzzzzzzzzzzzzzzz!"

echo "  [PASS] closure_args_borrowed: closure calls free owned arguments; what a closure keeps it takes for itself"
