#!/bin/sh
# A closure in a module's function captures that function's parameter and
# passes it on as a value: it stays the parameter when the program that
# imports the module defines a function of the same name.
#
# After module merging the program holds the consumer's functions beside the
# module's, and codegen asks "is this identifier a top-level function?" before
# lowering a `fn` argument. #1657 made a parameter win over that in the
# function's own body; inside a closure the capture was not registered as a
# binding of the closure body, so the consumer's function won there:
#
#   lib.reorder  `apply(cb: fn, x)` runs `|n| { call2(cb, n, x) }`; the
#                program defines its own `cb(x, y)`. The module ran the
#                program's `cb` in place of the caller's closure, the
#                env dropped (aether-ui's `reorderable(cb: fn ...)` beside a
#                program's `cb`: a SIGSEGV through the mismatched adapter).
#
# The reverse, a program's parameter beside a module's exported function of
# that name, is pinned too, and the program's own `cb` still runs when called.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=closure_capture_shadows_consumer_fn

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] $NAME: $AE not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export AETHER_HOME="$ROOT"
fails=0

# run_ok <case> <want>: main.ae runs and prints <want> (its lines joined by
# '|').
run_ok() {
    got=$(cd "$TMP" && "$AE" run main.ae 2>&1 | tr -d '\r' | grep -v '^[[:space:]]*$' | paste -sd'|' -)
    if [ "$got" != "$2" ]; then
        echo "  [FAIL] $NAME/$1: expected '$2', got '$got'"
        fails=$((fails + 1))
    fi
}

mkdir -p "$TMP/lib/reorder" "$TMP/lib/named"
cat > "$TMP/lib/reorder/module.ae" <<'AE'
exports(apply, apply_nested)

run_it(f: fn) {
    f(1)
}

call2(g: fn, a: int, b: int) {
    g(a, b)
}

apply(cb: fn, x: int) {
    run_it(|n: int| { call2(cb, n, x) })
}

apply_nested(cb: fn, x: int) {
    run_it(|n: int| {
        run_it(|m: int| { call2(cb, n + m, x) })
    })
}
AE

cat > "$TMP/lib/named/module.ae" <<'AE'
exports(cb)

cb(x: int, y: int) {
    println("module cb ${x} ${y}")
}
AE

# The module's captured parameter beside the program's function.
cat > "$TMP/main.ae" <<'AE'
import lib.reorder

cb(x: int, y: int) {
    println("program cb ${x} ${y}")
}

main() {
    reorder.apply(|a: int, b: int| { println("closure ${a} ${b}") }, 3)
    reorder.apply_nested(|a: int, b: int| { println("nested ${a} ${b}") }, 3)
    cb(7, 8)
}
AE
run_ok module_param_beside_program_fn "closure 1 3|nested 2 3|program cb 7 8"

# The program's captured parameter beside the module's function.
cat > "$TMP/main.ae" <<'AE'
import lib.named

run_it(f: fn) {
    f(1)
}

call2(g: fn, a: int, b: int) {
    g(a, b)
}

apply(cb: fn, x: int) {
    run_it(|n: int| { call2(cb, n, x) })
}

main() {
    apply(|a: int, b: int| { println("closure ${a} ${b}") }, 3)
    named.cb(7, 8)
}
AE
run_ok program_param_beside_module_fn "closure 1 3|module cb 7 8"

if [ "$fails" -ne 0 ]; then
    exit 1
fi
echo "  [PASS] $NAME: a closure's capture stays the parameter beside a consumer's function"
exit 0
