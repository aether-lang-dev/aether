#!/bin/sh
# A function's fn-typed parameter is that function's (#2130).
#
# Codegen keeps a registry of fn-pointer-typed locals and parameters so a
# call through one is emitted with the right C function-pointer cast. The
# registry was never emptied between functions: a module function's
# parameter `run: fn(int, int, int, ptr)` stayed registered for the rest
# of the translation unit, and an unrelated program function
# `run(name, steps)` called later was emitted as
# `((void (*)(int, int, int, void*))&run)("hello", 3)` — a C error, or
# a miscompile when the arity happened to match. The registry is now
# per function (and per main / receive handler); a closure still sees its
# enclosing function's fn-typed locals.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] fnptr_param_name_scope: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT

mkdir -p "$tmp/mylib"
cat > "$tmp/mylib/module.ae" <<'AE'
exports ( for_each )
for_each(run: fn(int, int, int, ptr) -> void, count: int, context: ptr) {
    i = 0
    while i < count { run(i, i + 1, 0, context); i = i + 1 }
}
AE
cat > "$tmp/main.ae" <<'AE'
import mylib

run(name: string, steps: int) { println("${name} ${steps}") }
work(a: int, b: int, c: int, ctx: ptr) { println("work ${a} ${b}") }
twice(step: fn(int) -> int, x: int) -> int {
    // A closure inside the function still calls the parameter through its type.
    f = |v: int| { return step(step(v)) }
    return f(x)
}
inc(v: int) -> int { return v + 1 }
helper(x: int) -> int { return x * 10 }

message Go { x: int }
actor A {
    // A closure in a state initializer calls a program function: the callee
    // is not a capture (the capture analysis has no enclosing function here).
    state handler = |x: int| { return helper(x) }
    receive { Go(x) -> { println("${call(handler, x)}") } }
}

// A multi-clause function is its own C function: the module's `run`
// parameter must not reach into it either.
count(0) -> { run("zero", 0); return 0 }
count(n) -> { return n + count(n - 1) }

main() {
    mylib.for_each(work, 2, null)
    run("hello", 3)
    println("${twice(inc, 5)}")
    // The program's own `run` again after a function that has a `step` parameter.
    run("again", 1)
    a = spawn(A())
    a ! Go { x: 5 }
    sleep(100)
    println("${count(3)}")
}
AE
want='work 0 1
work 1 2
hello 3
7
again 1
50
zero 0
6'
got="$(cd "$tmp" && AETHER_LIB_DIR="$tmp" AETHER_HOME="$ROOT" "$AE" run main.ae 2>&1)"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] fnptr_param_name_scope: output differs"
    printf '%s\n' "$got" | head -8 | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] fnptr_param_name_scope: a module's fn-typed parameter name does not capture an unrelated program function"
exit 0
