#!/bin/sh
# A closure the program did not compile may keep its argument, so a call
# through an `fn` parameter frees nothing when one can be called (#2499).
#
# A closure call borrows its arguments only when the compiler has seen the
# body of every closure the program can call and none keeps a parameter.
# A closure can also come from outside: a box a host bridge or another
# library made, recovered with `unbox_closure` or by passing a `ptr` to an
# `fn` slot; a closure an extern returns; one C passes to a @c_callback;
# one C stores in a struct the program reads (an extern struct, a struct an
# extern fills, a raw pointer viewed as such a struct); and in a library
# build, whatever the host passes to an exported function. In each case the
# owned argument must stay alive: this checks the emitted C does not free it,
# against a control program that does.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AETHERC" ] && [ ! -x "$AETHERC.exe" ]; then
    echo "  [SKIP] closure_args_foreign: $AETHERC not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

fail() {
    echo "  [FAIL] closure_args_foreign: $1"
    [ -n "$2" ] && [ -f "$2" ] && sed 's/^/        /' "$2" | head -20
    exit 1
}

# gen <name> <decls> <body> [aetherc flags]: a program whose only closure
# reads its parameter, plus `decls` at the top and `body` in main.
gen() {
    cat > "$tmp/$1.ae" <<AE
import std.string

$2

mk(a: string) -> string {
    return string.concat(a, "!")
}

apply_owned(f: fn, a: string) {
    call(f, mk(a))
}

main() {
    look = | s: string | { println(s) }
    apply_owned(look, "x")
    $3
}
AE
    "$AETHERC" $4 "$tmp/$1.ae" "$tmp/$1.c" >"$tmp/$1.log" 2>&1 || fail "$1: aetherc failed" "$tmp/$1.log"
}

freed() {
    grep -q '_ad_[0-9]* = (char\*)(mk(a)); .*f\.fn)(f\.env, _ad_[0-9]*); aether_heap_str_free' "$tmp/$1.c"
}

gen control "" ""
freed control || fail "control: the argument is not freed, so this test proves nothing" "$tmp/control.c"

gen unbox "" 'g = unbox_closure(box_closure(look))
    apply_owned(g, "y")'
gen ptr_slot "" 'p = box_closure(look)
    apply_owned(p, "y")'
gen extern_fn 'extern host_handler() -> fn' ""
gen c_callback '@c_callback
on_event(cb: fn) {
    call(cb, 1)
}' ""
gen extern_struct 'extern struct Hooks {
    cb: fn
}' ""
gen extern_fills 'struct Holder {
    cb: fn
}

extern fill_holder(h: *Holder)' ""
gen ptr_view 'struct Holder {
    cb: fn
}

extern host_ptr() -> ptr' 'h = host_ptr() as *Holder
    println(h == null)'
gen library "" "" "--emit=lib"

for c in unbox ptr_slot extern_fn c_callback extern_struct extern_fills ptr_view library; do
    if freed "$c"; then
        fail "$c: a closure from outside the program can be called, yet the argument is freed" "$tmp/$c.c"
    fi
done
echo "  [PASS] closure_args_foreign: no argument is freed when a closure from outside the program can be called"
