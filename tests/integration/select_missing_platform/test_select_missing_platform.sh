#!/bin/sh
# Regression (#3002): a select() with no value for a platform is a coded
# error that stops the build.
#
# `select(linux: 8080)` printed "error: select() at line N: missing platform
# without 'other:' fallback." from codegen, with no code, and the build went
# on into C, which stopped at the `#error` emitted in the missing value's
# place. The generated C is the same for every platform, so a value missing
# for one is missing for the program, whatever it is built for: E0701, from
# the type checker, naming the platforms with no value and the branches
# given, and no C is written. A select with every platform, with `other:`,
# or whose build-symbol key is defined still builds.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] select_missing_platform: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0

printf 'main() {\n    port = select(linux: 8080)\n    println(port)\n}\n' > "$TMP/missing.ae"
out="$("$AE" build "$TMP/missing.ae" -o "$TMP/missing.bin" 2>&1)"
rc=$?
first=$(printf '%s\n' "$out" | grep -a -m1 '^error')
at=$(printf '%s\n' "$out" | grep -a -m1 -- '-->' | sed 's/.*\.ae://')
want='error[E0701]: select() has no value for windows and macos and no `other:` fallback; its branches are `linux:`'
if [ $rc -eq 0 ]; then
    echo "  [FAIL] missing: built"
    fail=1
elif [ "$first" != "$want" ] || [ "$at" != "2:12" ]; then
    echo "  [FAIL] missing: expected '$want' at 2:12, got '$first' at $at"
    fail=1
elif printf '%s\n' "$out" | grep -a -q -E '#error|^error: select'; then
    echo "  [FAIL] missing: the build went on to C:"
    printf '%s\n' "$out" | sed 's/^/        /' | head -8
    fail=1
elif ! printf '%s\n' "$out" | grep -a -qF 'help: add `windows:` and `macos:`, or `other:`'; then
    echo "  [FAIL] missing: the help does not name the branches to add"
    fail=1
else
    echo "  [PASS] missing: E0701 at the select, naming windows and macos, and no C"
fi

printf 'main() {\n    a = select(MY_SYMBOL: 1)\n    println(a)\n}\n' > "$TMP/symbol.ae"
out="$("$AE" check "$TMP/symbol.ae" 2>&1)"
if [ $? -eq 0 ]; then
    echo "  [FAIL] symbol_undefined: a select of an undefined symbol alone compiled"
    fail=1
elif ! printf '%s\n' "$out" | grep -a -qF 'no value for linux, windows and macos'; then
    echo "  [FAIL] symbol_undefined: the error does not name the three platforms:"
    printf '%s\n' "$out" | grep -a '^error' | sed 's/^/        /'
    fail=1
else
    echo "  [PASS] symbol_undefined"
fi
out="$("$AE" run -D MY_SYMBOL "$TMP/symbol.ae" 2>&1)"
if [ "$out" = "1" ]; then
    echo "  [PASS] symbol_defined"
else
    echo "  [FAIL] symbol_defined: expected 1, got: $out"
    fail=1
fi

cat > "$TMP/good.ae" <<'AEOF'
main() {
    a = select(windows: 1, other: 2)
    b = select(linux: 1, windows: 2, macos: 3)
    c = select(AE_NEVER_DEFINED: 5, windows: 1, other: 2)
    println(a + b + c)
}
AEOF
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) want=4 ;;
    Darwin) want=7 ;;
    *) want=5 ;;
esac
out="$("$AE" run "$TMP/good.ae" 2>&1)"
case "$out" in
    "$want") echo "  [PASS] good" ;;
    *)
        echo "  [FAIL] good: expected $want, got:"
        printf '%s\n' "$out" | sed 's/^/        /' | head -8
        fail=1
        ;;
esac

# A program's own `select()` with no parameters is not the platform select:
# the rule asks only a call with arguments, as codegen lowers only those.
printf 'select() -> int {\n    return 99\n}\n\nmain() {\n    n = select()\n    println(n)\n}\n' > "$TMP/own.ae"
out="$("$AE" run "$TMP/own.ae" 2>&1 | tr -d '\r' | tail -1)"
if [ "$out" = "99" ]; then
    echo "  [PASS] own_select: a program's select() with no parameters runs"
else
    echo "  [FAIL] own_select: expected 99, got: $out"
    fail=1
fi

# A bare select() with no function of that name is the builtin with no
# branches: E0701 before C, not an undeclared ae_select in the C compiler.
printf 'main() {\n    n = select()\n    println(n)\n}\n' > "$TMP/bare.ae"
out="$("$AE" check "$TMP/bare.ae" 2>&1)"
if printf '%s\n' "$out" | grep -q "error\[E0701\]"; then
    echo "  [PASS] bare_select: a select() with no branches and no function of that name is E0701"
else
    echo "  [FAIL] bare_select: expected E0701, got:"
    printf '%s\n' "$out" | head -4 | sed 's/^/        /'
    fail=1
fi

exit $fail
