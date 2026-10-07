#!/bin/sh
# A heap-string argument to a closure call is freed after the call, unless
# the closure's parameter keeps it (#2493).
#
# `call(f, mk("x"))` and `f(mk("x"))` on a closure local passed the argument
# straight to the closure, so nothing freed it; a named call hoists it into
# a temp and frees it after the call. The closure call now gets the same
# wrap, decided by the closure literal's body. This test checks the emitted
# C for both spellings, and that an argument the closure keeps (stored in a
# captured list, captured by a nested closure that outlives the call) is not
# freed under it. tests/regression/test_closure_call_frees_heap_arg.ae runs
# the ownership cases under the leaks gate.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] closure_call_frees_heap_arg: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

fail() {
    echo "  [FAIL] closure_call_frees_heap_arg: $1"
    [ -n "$2" ] && [ -f "$2" ] && sed 's/^/        /' "$2" | head -20
    exit 1
}

cat > "$tmp/main.ae" <<'AE'
import std.string
import std.list

mk(a: string) -> string {
    return string.concat(a, "!")
}

main() {
    f = | s: string | { println(s) }
    call(f, mk("x"))
    f(mk("y"))

    // Kept: a captured list holds the parameter.
    kept = list.new()
    keep = | s: string | {
        list.add(kept, s)
    }
    keep(mk("k1"))
    call(keep, mk("k2"))
    a, _ = list.get(kept, 0)
    b, _ = list.get(kept, 1)
    println("${a} ${b}")

    // Kept: a nested closure captures the parameter and is returned.
    hold = | s: string | {
        return || { println("held ${s}") }
    }
    fn later = call(hold, mk("h"))
    call(later)
}
AE

"$AE" run "$tmp/main.ae" >"$tmp/out.log" 2>&1 || fail "program did not run" "$tmp/out.log"
want="x!
y!
k1! k2!
held h!"
got="$(tr -d '\r' < "$tmp/out.log")"
[ "$got" = "$want" ] || fail "wrong output" "$tmp/out.log"

"$AETHERC" "$tmp/main.ae" "$tmp/main.c" >"$tmp/gen.log" 2>&1 || fail "aetherc failed" "$tmp/gen.log"
# Both spellings of the read-only call free their temp.
n="$(grep -c 'char\* _ad_[0-9]* = (char\*)(mk("[xy]")); _closure_fn_0(.*); aether_heap_str_free(_ad_' "$tmp/main.c")"
[ "$n" = "2" ] || fail "a read-only closure argument is not freed after the call ($n of 2)" "$tmp/main.c"
# The kept arguments are passed through, not freed.
if grep -q 'mk("k[12]")); _closure_fn' "$tmp/main.c" || grep -q 'mk("h")); _closure_fn' "$tmp/main.c"; then
    fail "an argument the closure keeps is freed after the call"
fi
echo "  [PASS] closure_call_frees_heap_arg: closure calls free read-only heap arguments and leave kept ones"
