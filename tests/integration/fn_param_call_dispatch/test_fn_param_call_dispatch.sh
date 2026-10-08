#!/bin/sh
# #2513: a call on an `fn` parameter dispatches through the parameter's
# value.
#
# Codegen records which closure literal a variable holds so `call(f)` can
# call that closure's C function directly. The record was keyed by the
# name alone, so `f()` on the `fn` parameter of `apply` resolved to the
# closure bound to a local `f` of an unrelated function and ran its code
# with `apply`'s argument as the env: an access violation. A variable is
# now named by the scope that declares it as well; a parameter, or a
# variable bound in another scope, goes through the value (`f.fn(f.env)`).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] fn_param_call_dispatch: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
apply(f: fn) { f() }

twice(f: fn) -> int { return call(f) + call(f) }

maker() -> fn {
    n = 5
    f = || { println("maker's f ${n}") }
    return f
}

counter() -> fn {
    c = 0
    f = || {
        c = c + 1
        return c
    }
    return f
}

main() {
    apply(|| { println("passed closure") })
    g = maker()
    call(g)
    f = counter()
    println("twice ${twice(f)}")
    h = || { return 7 }
    println("twice h ${twice(h)}")
    apply(g)
    k = |f: fn| -> call(f)
    println("k ${call(k, h)}")
}
AE

want="passed closure
maker's f 5
twice 3
twice h 14
maker's f 5
k 7"
got="$("$AE" run "$tmp/main.ae" 2>&1 | tr -d '\r' | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] fn_param_call_dispatch: a call on an fn parameter did not go through its value"
    printf 'got:\n%s\nwant:\n%s\n' "$got" "$want" | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] fn_param_call_dispatch: a call on an fn parameter dispatches through the parameter's value"
