#!/bin/sh
# Regression (#3708): a `const` or `var` name declared twice at the top of one
# file must not compile.
#
# Nothing diagnosed it. In a module the merge kept the first declaration and
# dropped the second without a word, so every use read the value its author
# had replaced (ae3d's LIMP_FRICTION, 0.1 where 0.3 was meant). In a program
# both reached the generated C, and only the C compiler objected, naming a
# line of generated C.
#
# Each repeat is refused with one E0400 at the second declaration, naming the
# line of the first: two consts, two vars, a const and a var, two that agree
# in value, and a const repeated in an imported module. What stays legal: one
# name per arm of a top-level `when`, and the same name in two modules.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AETHERC" ] && [ ! -x "$AETHERC.exe" ]; then
    echo "  [SKIP] top_level_name_twice: aetherc not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0

# refuse DIR FILE WHERE MESSAGE: $TMP/DIR/main.ae is refused with one error,
# error[E0400] whose text contains MESSAGE, at FILE:WHERE (line:column).
refuse() {
    dir="$1"; file="$2"; where="$3"; msg="$4"
    out="$(cd "$TMP/$dir" && "$AETHERC" --check main.ae 2>&1)"
    rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "  [FAIL] $dir: compiled"
        fail=1
        return
    fi
    errors=$(printf '%s\n' "$out" | grep -c '^error')
    first=$(printf '%s\n' "$out" | grep -m1 '^error')
    at=$(printf '%s\n' "$out" | grep -m1 -- '-->' | sed 's/.*--> *//; s/.*[\\/]//')
    case "$first" in
        "error[E0400]: "*"$msg"*) ;;
        *)
            echo "  [FAIL] $dir: expected error[E0400] with \"$msg\", got: $first"
            printf '%s\n' "$out" | sed 's/^/        /' | head -10
            fail=1
            return
            ;;
    esac
    if [ "$at" != "$file:$where" ]; then
        echo "  [FAIL] $dir: reported at $at, expected $file:$where"
        fail=1
        return
    fi
    if [ "$errors" -ne 1 ]; then
        echo "  [FAIL] $dir: expected 1 error, got $errors"
        printf '%s\n' "$out" | sed 's/^/        /' | head -20
        fail=1
        return
    fi
    echo "  [PASS] $dir"
}

# accept DIR: $TMP/DIR/main.ae compiles.
accept() {
    dir="$1"
    out="$(cd "$TMP/$dir" && "$AETHERC" --check main.ae 2>&1)"
    if [ $? -ne 0 ]; then
        echo "  [FAIL] $dir: refused"
        printf '%s\n' "$out" | sed 's/^/        /' | head -10
        fail=1
        return
    fi
    echo "  [PASS] $dir"
}

prog() { mkdir -p "$TMP/$1"; printf "$2" > "$TMP/$1/main.ae"; }

prog two_consts 'const X = 1\nconst X = 2\nmain() {\n    println("${X}")\n}\n'
refuse two_consts main.ae 2:1 "'X' is declared twice at the top of this file: \`const X\` here, and \`const X\` at line 1"

prog two_vars 'var g = 1\nmain() {\n    println("${g}")\n}\nvar g = 2\n'
refuse two_vars main.ae 5:1 "\`var g\` here, and \`var g\` at line 1"

prog const_and_var 'const N = 4\nvar N = 5\nmain() {\n    println("${N}")\n}\n'
refuse const_and_var main.ae 2:1 "\`var N\` here, and \`const N\` at line 1"

prog same_value 'const K = 7\nconst K = 7\nmain() {\n    println("${K}")\n}\n'
refuse same_value main.ae 2:1 "'K' is declared twice"

# The module case from the report: the second declaration was dropped.
mkdir -p "$TMP/in_module/m"
printf 'const LIMP = 0.1\nshow() -> float {\n    return LIMP\n}\nconst LIMP = 0.3\n' > "$TMP/in_module/m/module.ae"
printf 'import m\nmain() {\n    println("${m.show()}")\n}\n' > "$TMP/in_module/main.ae"
refuse in_module module.ae 5:1 "\`const LIMP\` here, and \`const LIMP\` at line 1"

prog per_when_arm 'when target.os == "windows" {\n    const SEP = "w"\n} else {\n    const SEP = "u"\n}\nmain() {\n    println(SEP)\n}\n'
accept per_when_arm

mkdir -p "$TMP/two_modules/a" "$TMP/two_modules/b"
printf 'const V = 1\nget() -> int {\n    return V\n}\n' > "$TMP/two_modules/a/module.ae"
printf 'const V = 2\nget() -> int {\n    return V\n}\n' > "$TMP/two_modules/b/module.ae"
printf 'import a\nimport b\nmain() {\n    println("${a.get() + b.get()}")\n}\n' > "$TMP/two_modules/main.ae"
accept two_modules

[ "$fail" -eq 0 ] || exit 1
echo "  [PASS] top_level_name_twice: a name declared twice at the top of a file is refused"
