#!/bin/sh
# Regression (#3524): an integer division or remainder by zero in the
# initializer of a `const` or a module `var` is a compile error that names
# the expression, each name as it was written. `const B = 10 / 0` reached the
# C compiler, which refused the static's initializer or folded it to
# anything. The compiler computes the initializer, literals, `const`s and the
# operators over them in their own width (`65536 * 65536` is 0 in an
# `int`), and refuses it there.
#
# Anywhere else the same division is code, which may be guarded or never
# run: as in Java it is no constant expression, so it compiles, and it panics
# with division_by_zero if it runs. That holds in a function and in an
# imported module, where a refusal failed the build of every program that
# imported it. A local or parameter named like a constant is that binding,
# not the constant.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
[ -x "$AETHERC" ] || AETHERC="$ROOT/build/aetherc.exe"
AE="$ROOT/build/ae"
[ -x "$AE" ] || AE="$ROOT/build/ae.exe"
NAME=constant_division_by_zero

if [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] $NAME: $AETHERC not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT
export AETHER_HOME="$ROOT"
fail=0

# expect <name> <message fragment> <program>
expect() {
    printf '%s\n' "$3" > "$TMPDIR/$1.ae"
    if "$AETHERC" --check "$TMPDIR/$1.ae" > "$TMPDIR/$1.out" 2>&1; then
        echo "  [FAIL] $NAME: $1 compiled"
        fail=1
    elif ! grep -qF -- "$2" "$TMPDIR/$1.out"; then
        echo "  [FAIL] $NAME: $1 did not say: $2"
        sed 's/^/    /' "$TMPDIR/$1.out" | head -6
        fail=1
    fi
}

# run <dir> <main.ae> <expected output>: builds and runs a program of its own
# directory (a module beside it under lib/).
run() {
    if ! (cd "$1" && "$AE" build "$2" -o "$1/prog" > "$1/build.log" 2>&1); then
        echo "  [FAIL] $NAME: $1/$2 was refused"
        sed 's/^/    /' "$1/build.log" | grep -v "^    Building" | head -8
        fail=1
        return
    fi
    out=$("$1/prog" 2>&1 | tr -d '\r' | sed 's/ at .*//')
    if [ "$out" != "$3" ]; then
        echo "  [FAIL] $NAME: $1/$2 printed other values"
        printf '%s\n' "$out" | sed 's/^/    /'
        fail=1
    fi
}

expect const_init "division by zero in the constant expression \`10 / 0\`" \
'const B = 10 / 0
main() { println("${B}") }'

expect named_const "remainder by zero in the constant expression \`7 % Z\`" \
'const Z = 0
const C = 7 % Z
main() { println("${C}") }'

expect nested "division by zero in the constant expression \`10 / (5 - 5)\`" \
'const X = 10 / (5 - 5)
main() { println("${X}") }'

expect long_divisor "division by zero in the constant expression \`5000000000 / (N - 4)\`" \
'const N = 4
const L = 5000000000 / (N - 4)
main() { println("${L}") }'

expect wraps_to_zero "division by zero in the constant expression \`10 / (65536 * 65536)\`" \
'const W0 = 10 / (65536 * 65536)
main() { println("${W0}") }'

expect module_var "division by zero in the constant expression \`1 / Z\`" \
'const Z = 0
var g = 1 / Z
main() { println("${g}") }'

# A module's constant initializer: named as its module wrote it, not by the
# program's merged name (`100 / cfg_D`); in the program, as it wrote it.
mkdir -p "$TMPDIR/libinit/lib"
printf 'exports(E)\n\nconst D = 0\nconst E = 100 / D\n' > "$TMPDIR/libinit/lib/cfg.ae"
printf 'import lib.cfg\n\nmain() { println("${cfg.E}") }\n' > "$TMPDIR/libinit/main.ae"
if (cd "$TMPDIR/libinit" && "$AETHERC" --check main.ae > out.txt 2>&1); then
    echo "  [FAIL] $NAME: a module constant dividing by zero compiled"
    fail=1
elif ! grep -qF -- "division by zero in the constant expression \`100 / D\`" "$TMPDIR/libinit/out.txt"; then
    echo "  [FAIL] $NAME: the module constant's error does not name \`100 / D\`"
    sed 's/^/    /' "$TMPDIR/libinit/out.txt" | head -6
    fail=1
fi
mkdir -p "$TMPDIR/libuse/lib"
printf 'exports(D)\n\nconst D = 0\n' > "$TMPDIR/libuse/lib/cfg.ae"
printf 'import lib.cfg\n\nconst R = 100 %% cfg.D\n\nmain() { println("${R}") }\n' > "$TMPDIR/libuse/main.ae"
if (cd "$TMPDIR/libuse" && "$AETHERC" --check main.ae > out.txt 2>&1); then
    echo "  [FAIL] $NAME: a constant dividing by a module's zero compiled"
    fail=1
elif ! grep -qF -- "remainder by zero in the constant expression \`100 % cfg.D\`" "$TMPDIR/libuse/out.txt"; then
    echo "  [FAIL] $NAME: the error does not name \`100 % cfg.D\`"
    sed 's/^/    /' "$TMPDIR/libuse/out.txt" | head -6
    fail=1
fi

# Code compiles, guarded or not: a branch that never runs, a constant
# divisor that is not 0 (a shift count is masked, so 1 << 32 is 1),
# MIN / -1, a name bound like a constant, and a division that runs, which
# panics.
mkdir -p "$TMPDIR/accept"
cat > "$TMPDIR/accept/main.ae" <<'AE'
const D = 0
const ZERO = 0
const W = 10 / 2
const WRAP = (-2147483647 - 1) / -1

by(ZERO: int) -> int { return 10 / ZERO }

main() {
    if D != 0 {
        println(100 / D)
    }
    ZERO = 3
    a = 10 / ZERO
    b = 10 / (1 << 32)
    try {
        println("${10 / (5 - 5)}")
    } catch e {
        println("caught ${e}")
    }
    println("${W} ${WRAP} ${a} ${b} ${by(5)}")
}
AE
run "$TMPDIR/accept" main.ae "caught division_by_zero: division by zero
5 -2147483648 3 10 2"

# A module whose code divides by its own zero constant, guarded: every
# program that imports it and calls it builds.
mkdir -p "$TMPDIR/lib/lib"
cat > "$TMPDIR/lib/lib/cfg.ae" <<'AE'
exports(ratio)

const D = 0

ratio() -> int {
    if D == 0 { return 1 }
    return 100 / D
}
AE
printf 'import lib.cfg\n\nmain() { println("ratio ${cfg.ratio()}") }\n' > "$TMPDIR/lib/main.ae"
run "$TMPDIR/lib" main.ae "ratio 1"

[ "$fail" = 0 ] && echo "  [PASS] $NAME: a constant's initializer dividing by zero is refused, naming it as written; code compiles"
exit $fail
