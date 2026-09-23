#!/bin/sh
# Regression (#2173): a program's local variable must not retype a module's
# extern of the same name.
#
# Type inference keeps one flat symbol table per program: a function's locals
# are pushed for its walk and popped after it. A local named like something
# already in the table -- here the `extern floor(x: float) -> float` a module
# declares -- retyped that entry in place instead of shadowing it, and the
# pop cannot undo an overwrite. So `floor = new_thing()` in one of the
# program's own functions left the module's extern typed `ptr`, and the
# module's `floor(x) as int` failed to compile ("cannot cast ptr to int"),
# in a file its author never touched. #1967 fixed the same thing for
# parameters. A local in main was worse: main's locals stayed in the table
# after its walk, so everything checked after it resolved names to them
# (#2186).
set -eu

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
AE="$ROOT/build/ae"
if [ ! -x "$AE" ]; then
    echo "  [SKIP] local_shadows_module_extern: build/ae not built"
    exit 0
fi
AETHER_HOME=""
export AETHER_HOME

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fail() {
    echo "  [FAIL] local_shadows_module_extern: $1"
    exit 1
}

# The module, reached through a second module as in the report.
mkdir -p "$WORK/noise" "$WORK/core"
cat > "$WORK/noise/module.ae" <<'AE'
exports(cell, down)

extern floor(x: float) -> float

cell(x: float) -> int {
    ix = floor(x) as int
    return ix
}

// A float result, so an int-typed extern would show as a wrong value.
down(x: float) -> float {
    return floor(x)
}
AE
cat > "$WORK/core/module.ae" <<'AE'
import noise
exports(sample, lower)

sample(x: float) -> int {
    return noise.cell(x)
}

lower(x: float) -> float {
    return noise.down(x)
}
AE

# Each program binds `floor` a different way; every one must compile and
# leave the module's floor(2.7) == 2 and floor(-0.5) == -1.0.
case_program() {
    name="$1"
    body="$2"
    cat > "$WORK/$name.ae" <<AE
import core

new_thing() -> ptr {
    return null
}

pair() -> (ptr, int) {
    return null, 3
}

$body

main() {
    run()
    if core.sample(2.7) != 2 { println("FAIL cell"); exit(1) }
    if core.lower(-0.5) != -1.0 { println("FAIL down: \${core.lower(-0.5)}"); exit(1) }
    println("PASS")
}
AE
    ( cd "$WORK" && "$AE" run "$name.ae" > "$WORK/$name.log" 2>&1 ) \
        || { sed 's/^/    /' "$WORK/$name.log" | head -20; fail "$name: the program did not build or run"; }
    [ "$(tail -1 "$WORK/$name.log")" = "PASS" ] \
        || { sed 's/^/    /' "$WORK/$name.log" | head -20; fail "$name: expected PASS"; }
}

case_program ptr_local 'extern exit(code: int)
run() {
    floor = new_thing()
    if floor != null { exit(1) }
}'

case_program int_local 'extern exit(code: int)
run() {
    floor = 7
    if floor != 7 { exit(1) }
}'

case_program destructure_target 'extern exit(code: int)
run() {
    floor, n = pair()
    if floor != null || n != 3 { exit(1) }
}'

# A local in main itself.
cat > "$WORK/in_main.ae" <<'AE'
import core
extern exit(code: int)

main() {
    floor = "a string"
    if floor != "a string" { exit(1) }
    if core.sample(2.7) != 2 { println("FAIL cell"); exit(1) }
    if core.lower(-0.5) != -1.0 { println("FAIL down"); exit(1) }
    println("PASS")
}
AE
( cd "$WORK" && "$AE" run in_main.ae > "$WORK/in_main.log" 2>&1 ) \
    || { sed 's/^/    /' "$WORK/in_main.log" | head -20; fail "in_main: the program did not build or run"; }
[ "$(tail -1 "$WORK/in_main.log")" = "PASS" ] \
    || { sed 's/^/    /' "$WORK/in_main.log" | head -20; fail "in_main: expected PASS"; }

echo "  [PASS] local_shadows_module_extern: a program's local shadows, never retypes, a module's extern"
