#!/bin/bash
# Regression test: defer + multi-value return.
#
# Bug: codegen for `return a, b` (or any multi-value return) inside
# a function that has any active `defer` saved the FIRST slot's
# value into _builder_ret typed as the first slot's type, then
# emitted `return _builder_ret;`. For a function returning
# (string, string, int), the C compiler then sees:
#
#   string _builder_ret = some_string_expr;
#   ...
#   return _builder_ret;   // expects _tuple_string_string_int
#
# and rejects with "incompatible types when returning type 'string'
# but '_tuple_string_string_int' was expected".
#
# Fix in compiler/codegen/codegen_stmt.c: when the return statement
# has multi-value children, emit the C-side tuple literal directly,
# skipping the typed-temp dance that single-value returns use.
#
# Surfaced during PR #255's head()/patch() wrappers — the natural
# `defer request_free(req)` form didn't compile, so those wrappers
# carry manual cleanup at each return site as a workaround. With
# this fix landed they can be cleaned up to use defer as intended.
set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
AETHER_ROOT="$(cd "$DIR/../../.." && pwd)"
AE="${AETHER_ROOT}/build/ae"

if [ ! -x "$AE" ]; then
    echo "SKIP: $AE not built"
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK" || true' EXIT

# Three repros, each a different multi-value-return shape paired
# with defer. All three should compile + run; pre-fix all three
# rejected with C-level "incompatible types when returning ...".
cat > "$WORK/main.ae" <<'AE'
extern exit(code: int)

// Track whether the deferred call ran. A simple int counter via
// a state actor would be cleaner but pulls in the actor system;
// using a global-ish hack via repeated calls works for a smoke
// test.
state_holder() -> int {
    return 0
}

cleanup_marker() {
    println("cleanup ran")
}

// Shape 1: 2-tuple (string, string) with defer.
two_tuple_with_defer(input: string) -> {
    defer cleanup_marker()
    if input == "early" { return "early-path", "" }
    return "happy-path", ""
}

// Shape 2: 3-tuple (int, string, string) — the head() shape.
three_tuple_with_defer(code: int) -> {
    defer cleanup_marker()
    if code < 0 { return -1, "", "negative-code" }
    if code == 0 { return 0, "", "" }
    return code, "value", ""
}

// Shape 3: 2-tuple where the deferred call is conditional via
// being declared inside an if-body. The pre-fix path bailed even
// in this nested-defer case.
nested_defer(input: string) -> {
    if input == "skip" { return "skipped", "" }
    defer cleanup_marker()
    return "ran", ""
}

main() {
    a, ae = two_tuple_with_defer("happy")
    if ae != "" { println("FAIL 1a: ${ae}"); exit(1) }
    if a != "happy-path" { println("FAIL 1a value: ${a}"); exit(1) }

    b, be = two_tuple_with_defer("early")
    if be != "" { println("FAIL 1b: ${be}"); exit(1) }
    if b != "early-path" { println("FAIL 1b value: ${b}"); exit(1) }

    c, cs, ce = three_tuple_with_defer(42)
    if ce != "" { println("FAIL 2a: ${ce}"); exit(1) }
    if c != 42 { println("FAIL 2a code: ${c}"); exit(1) }
    if cs != "value" { println("FAIL 2a slot: ${cs}"); exit(1) }

    d, ds, de = three_tuple_with_defer(-1)
    if de != "negative-code" { println("FAIL 2b: ${de}"); exit(1) }

    e, ee = nested_defer("ran")
    if ee != "" { println("FAIL 3a: ${ee}"); exit(1) }
    if e != "ran" { println("FAIL 3a: ${e}"); exit(1) }

    f, fe = nested_defer("skip")
    if fe != "" { println("FAIL 3b: ${fe}"); exit(1) }
    if f != "skipped" { println("FAIL 3b: ${f}"); exit(1) }

    println("PASS")
}
AE

cd "$WORK"
"$AE" build main.ae -o main >build.out 2>&1 || {
    cat build.out
    echo "FAIL: build rejected defer + multi-value return"
    exit 1
}

# Catch the codegen-warning-fallback signal too, just in case.
if grep -qi "incompatible types when returning" build.out; then
    cat build.out
    echo "FAIL: codegen emitted incompatible-types warning"
    exit 1
fi

out="$(./main 2>&1)" || {
    echo "FAIL: runtime error"
    echo "stdout: $out"
    exit 1
}
# Last line should be PASS; cleanup-ran lines come from the deferred
# calls (one per fn call that took the defer path).
last="$(echo "$out" | tail -1)"
if [ "$last" != "PASS" ]; then
    echo "FAIL: expected last line 'PASS', got '$last'"
    echo "full output: $out"
    exit 1
fi
echo "PASS"
