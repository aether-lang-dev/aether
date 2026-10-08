#!/bin/sh
# Consts are emitted in dependency order; a cycle is an Aether error (#2495).
#
# Codegen wrote each top-level const as a file-scope C definition in source
# order, and a C initializer can only name a definition above it, so
# `const HIGH = LOW << 4` ahead of `const LOW = 3` stopped the C compiler
# with "'ae_const_LOW' undeclared". The declarations are now put in
# dependency order (stable, so ordered programs are unchanged), across
# merged modules and module `var`s too. A const that depends on itself has
# no order and is reported at its declaration.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] const_dependency_order: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"
cd "$tmp" || exit 1

fail() {
    echo "  [FAIL] const_dependency_order: $1"
    [ -n "$2" ] && [ -f "$2" ] && sed 's/^/        /' "$2" | head -24
    exit 1
}

# run <file> <want>
run() {
    got="$("$AE" run "$1" 2>&1 | tr -d '\r' | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
    if [ "$got" != "$2" ]; then
        printf '  [FAIL] const_dependency_order: %s\ngot:\n%s\nwant:\n%s\n' \
            "$1" "$got" "$2" | sed '2,$s/^/        /'
        exit 1
    fi
}

# The issue's program.
cat > issue.ae <<'AE'
const HIGH = LOW << 4
const LOW = 3
main() {
    println("${HIGH}")
}
AE
run issue.ae "48"

# Forward references inside a module, across modules, and from a module
# `var`'s initializer.
mkdir -p top base
cat > base/module.ae <<'AE'
exports ( BASE, DERIVED )

const DERIVED = BASE * 10
const BASE = 5
AE
cat > top/module.ae <<'AE'
import base

exports ( TOP, MID )

const TOP = MID | base.BASE
const MID = base.BASE << 2
AE
cat > mods.ae <<'AE'
import top
import base

const FIRST = top.TOP + LATER
var counter = FIRST
const LATER = base.DERIVED - 1

main() {
    counter = counter + 1
    println("${FIRST} ${top.MID} ${base.DERIVED} ${counter}")
}
AE
run mods.ae "70 20 50 71"

# expect_cycle <file> <location> <message>
expect_cycle() {
    if "$AE" check "$1" > "$1.log" 2>&1; then
        fail "$1: a const cycle was accepted" "$1.log"
    fi
    tr -d '\r' < "$1.log" > "$1.txt"
    grep -qF -- "--> $2" "$1.txt" || fail "$1: not reported at $2" "$1.txt"
    grep -qF "$3" "$1.txt" || fail "$1: missing '$3'" "$1.txt"
    if grep -q "undeclared\|initializer element is not constant" "$1.txt"; then
        fail "$1: still reported by the C compiler" "$1.txt"
    fi
}

cat > cycle.ae <<'AE'
const A = B + 1
const B = A * 2
main() {
    println("${A}")
}
AE
expect_cycle cycle.ae "cycle.ae:1:1" "const 'A' depends on itself: A -> B -> A"

cat > self.ae <<'AE'
const A = A + 1
main() {
    println("${A}")
}
AE
expect_cycle self.ae "self.ae:1:1" "const 'A' depends on itself: A -> A"

echo "  [PASS] const_dependency_order: consts are defined before use in C; a const cycle is a type error"
