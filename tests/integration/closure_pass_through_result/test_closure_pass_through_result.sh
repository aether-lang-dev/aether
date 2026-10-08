#!/bin/sh
# A closure that returns another closure's call result is typed by where its
# own result is used, not int (#2484).
#
# `g = || { return call(f, x) }` with `f` an erased `fn` returned the int an
# erased call defaults to, so its C function was `static int` and a pointer
# came back cut to 32 bits: `let r: ptr = call(g)` truncated under `ae run`,
# and `return call(g)` from a `-> ptr` function did not compile ("makes
# pointer from integer"). A typed use of such a closure (a typed binding, a
# declared return) now types its return, through any number of pass-through
# levels. A closure no typed use reaches (handed to an extern, returned
# through `-> fn`) is still int, and is now warned about at its `return`.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] closure_pass_through_result: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

fail() {
    echo "  [FAIL] closure_pass_through_result: $1"
    [ -n "$2" ] && [ -f "$2" ] && sed 's/^/        /' "$2" | head -20
    exit 1
}

cat > "$tmp/typed.ae" <<'AE'
import std.string

// A pass-through closure returned from a `-> ptr` function.
pass_through(f: fn, x: ptr) -> ptr {
    g = || { return call(f, x) }
    return call(g)
}

// A typed binding of a pass-through closure's result.
via_binding(f: fn, x: ptr) -> ptr {
    g = || { return call(f, x) }
    let r: ptr = call(g)
    return r
}

// Two levels of pass-through, and a string result.
twice(f: fn, s: string) -> string {
    inner = || { return call(f, s) }
    outer = || { return call(inner) }
    string r = call(outer)
    return r
}

main() {
    p = "alpha"
    id = | x: ptr | { return x }
    println(pass_through(id, p) == p)
    println(via_binding(id, p) == p)
    up = | s: string | { return string.concat(s, "!") }
    println(twice(up, "hey"))
}
AE

"$AE" run "$tmp/typed.ae" >"$tmp/typed.log" 2>&1 || fail "the typed program did not run" "$tmp/typed.log"
want="true
true
hey!"
got="$(tr -d '\r' < "$tmp/typed.log")"
[ "$got" = "$want" ] || fail "wrong output, or a warning" "$tmp/typed.log"
"$AETHERC" "$tmp/typed.ae" "$tmp/typed.c" >"$tmp/gen.log" 2>&1 || fail "aetherc failed" "$tmp/gen.log"
if grep -q '^static int _closure_fn_' "$tmp/typed.c"; then
    fail "a pass-through closure is still emitted as returning int" "$tmp/typed.c"
fi

# The issue's program: the closure leaves through `-> fn`, so nothing types
# it. It is int as before, and the compiler says so at its return.
cat > "$tmp/erased.ae" <<'AE'
apply(f: fn, x: ptr) -> fn {
    return || { return call(f, x) }
}
main() {
    g = apply(| x: ptr | { return x }, "alpha")
    let r: ptr = call(g)
    println(r)
}
AE
"$AETHERC" "$tmp/erased.ae" "$tmp/erased.c" >"$tmp/erased.log" 2>&1 || fail "aetherc failed on the erased program" "$tmp/erased.log"
grep -q "this closure returns what a closure called through an erased \`fn\` returns" "$tmp/erased.log" \
    || fail "no warning for a closure whose result type cannot be known" "$tmp/erased.log"
grep -q "erased.ae:2:" "$tmp/erased.log" || fail "the warning does not point at the closure's return" "$tmp/erased.log"

echo "  [PASS] closure_pass_through_result: pass-through closures are typed by their use, and warned about when nothing types them"
