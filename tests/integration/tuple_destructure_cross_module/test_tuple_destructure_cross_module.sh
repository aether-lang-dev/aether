#!/bin/bash
# Regression test: tuple destructure with same local name across two
# imported modules should not produce E0200 "Type mismatch in variable
# initialization", and the caller should still see the destructured
# function's tuple-typed return.
#
# Bug history: upstream commit 7bc6c96 migrated std.map to Go-style
# tuple returns. aetherBuild/lib/build/module.ae and
# aetherBuild/lib/container/module.ae each declare a local `mod_name`
# (one as a string from a parameter assignment, the other as a ptr from
# a tuple destructure). With the symbol table pre-fix flat across all
# functions, the second module's `img_tag = mod_name` saw the wrong
# type and rejected with E0200.
#
# Fix: type_inference.c
#   - collect_function_constraints unwinds function-local symbols at
#     end-of-function so siblings don't see polluted types.
#   - resolve_local_var_type recognizes AST_TUPLE_DESTRUCTURE in
#     preceding siblings, so `return v` after `v, _ = some_call()`
#     resolves through the called function's tuple return type.
#   - infer_function_return_types is called inside the iteration loop
#     and re-publishes the freshly-inferred return types onto function
#     symbols so cross-function chains converge.
#   - infer_function_return_types re-tries when a function's previous
#     guess was VOID (otherwise an early UNKNOWN-resolved-as-VOID guess
#     sticks across iterations).
# Note: the test harness invokes this via `sh path/to/test.sh`, which
# resolves to dash on Ubuntu — dash does not implement `-o pipefail`,
# so this script avoids it. No pipelines are used below; `-eu` alone
# gives the safety we need (fail-fast on any non-zero, error on
# unset vars).
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

mkdir -p "$WORK/lib/build" "$WORK/lib/container"

cat > "$WORK/lib/build/module.ae" <<'AE'
import std.map

helper(s: string) {
    z = s
    return z
}
AE

cat > "$WORK/lib/container/module.ae" <<'AE'
import std.map

builder image(ctx: ptr) {
    z, _ = map.get(ctx, "module")
    y = z
    println(y)
}
AE

cat > "$WORK/main.ae" <<'AE'
import std.map
import build
import container

main() {
    m = map.new()
    _e = map.put(m, "module", "mod-A")
    container.image(m) {
    }
}
AE

cd "$WORK"
"$AE" build main.ae --lib lib -o main >build.out 2>&1 || {
    cat build.out
    echo "FAIL: build rejected the cross-module tuple-destructure"
    exit 1
}
out="$(./main 2>&1)"
if [ "$out" != "mod-A" ]; then
    echo "FAIL: expected 'mod-A', got '$out'"
    exit 1
fi
echo "PASS"
