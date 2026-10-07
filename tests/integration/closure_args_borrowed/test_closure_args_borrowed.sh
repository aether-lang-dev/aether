#!/bin/sh
# An owned string argument to a closure called through an `fn` parameter is
# freed after the call when no closure in the program keeps a parameter
# (#2499).
#
# `apply(f: fn, s: string) { call(f, s) }` has no closure body to read, so
# passing `s` on was counted as an escape and `apply(g, mk("x"))` leaked the
# string, as did `call(f, mk(a))` inside such a function. A closure call now
# borrows its arguments when every closure the program can call (each
# closure literal and each function used as a closure value) keeps none:
# a `string` parameter it captures is held through the env's own reference
# and one it returns is returned as a copy, while a store keeps it, as does
# a `ptr` parameter's capture or return. One keeping closure anywhere turns
# the convention off for the program, and the arguments are left alone as
# before. This checks the emitted C both ways, that every program prints
# what it stored, and that a struct literal returned with a parameter in it
# counts as keeping it (it did not, and the caller freed the field).

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
}
AE
run borrowed "held h!
h!+
held h!
h!+
8 e!"
grep -q '_ad_[0-9]* = (char\*)(mk("x")); apply(look, _ad_[0-9]*); aether_heap_str_free' "$tmp/borrowed.c" \
    || fail "the argument to apply() is not freed" "$tmp/borrowed.c"
n="$(grep -c 'char\* _ad_[0-9]* = (char\*)(mk(a)); .*f\.fn)(f\.env, _ad_[0-9]*); aether_heap_str_free' "$tmp/borrowed.c")"
[ "$n" = "3" ] || fail "an argument to a call through an fn parameter is not freed ($n of 3)" "$tmp/borrowed.c"

# ---- a closure keeps its parameter: nothing is freed under it --------------
for shape in list cell ptr branch; do
    case $shape in
        list)     keeper='kept = list.new()
    keep = | s: string | { list.add(kept, s) }'
                  reader='v, _ = list.get(kept, 0)
    println(v)' ;;
        cell)     keeper='last = ""
    keep = | s: string | { last = s }'
                  reader='println(last)' ;;
        ptr)      keeper='keep = | p: ptr | { return p }'
                  reader='println("ptr")' ;;
        branch)   keeper='kept = list.new()
    keep = | s: string | {
        if true {
            list.add(kept, s)
        }
    }'
                  reader='v, _ = list.get(kept, 0)
    println(v)' ;;
    esac
    cat > "$tmp/kept_$shape.ae" <<AE
import std.string
import std.list

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
}
AE
    case $shape in
        list|cell|branch) want="k!
zzzzzzzzzzzzzzzz!" ;;
        ptr) want="ptr
zzzzzzzzzzzzzzzz!" ;;
    esac
    run "kept_$shape" "$want"
    if grep -q 'mk("k")); apply(keep, _ad_' "$tmp/kept_$shape.c"; then
        fail "kept_$shape: an argument a closure keeps is freed" "$tmp/kept_$shape.c"
    fi
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
if grep -q 'mkrec(_ad_[0-9]*); aether_heap_str_free' "$tmp/struct_field.c"; then
    fail "an argument stored in a returned struct literal is freed" "$tmp/struct_field.c"
fi

echo "  [PASS] closure_args_borrowed: closure calls free owned arguments unless some closure keeps one"
