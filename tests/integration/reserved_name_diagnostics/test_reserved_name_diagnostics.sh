#!/bin/sh
# Reserved names and statement shapes are refused where they are written,
# naming what was meant (#2073, #2087's cousin, #2064's second half):
#
#   1. a user function named after a builtin (`isolate`) — used to compile
#      and be silently ignored: every call was lowered to the builtin;
#   2. a keyword as an assignment target (`when = 0.0`) — used to be
#      "Expected statement in block" pointing at the `=`;
#   3. `;` after a block-ending statement (`if x { a } ; b`) — used to be
#      "Expected statement in block" at the `;`; a `;` is the optional
#      terminator every simple statement already accepts, so it is now
#      accepted there too;
#   4. a closure passed where the parameter is a typed C function pointer
#      (`fn(int) -> int`) — used to reach the C compiler; a named function
#      still passes;
#   5. an error inside an imported module names the module's file — it
#      used to name the main program with the module's line numbers.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AETHERC" ] && [ ! -x "$AETHERC.exe" ]; then
    echo "  [SKIP] reserved_name_diagnostics: $AETHERC not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0

expect_error() {  # file, pattern, label
    out="$(AETHER_HOME="$ROOT" "$AETHERC" "$1" "$tmp/out.c" 2>&1)"
    if ! printf '%s' "$out" | grep -q "$2"; then
        echo "  [FAIL] reserved_name_diagnostics: $3"
        printf '%s\n' "$out" | grep -E "^error|-->" | head -4 | sed 's/^/        /'
        fail=1
    fi
}

# 1. builtin redefinition
cat > "$tmp/builtin.ae" <<'AE'
isolate(named: string) {
    println("never runs: ${named}")
}
main() {
    isolate("mesh")
}
AE
expect_error "$tmp/builtin.ae" "'isolate' is a builtin function and cannot be redefined" "a user function named isolate was not refused"
expect_error "$tmp/builtin.ae" "builtin.ae:1:1" "the builtin-redefinition error does not point at the definition"

# 2. keyword as an assignment target
cat > "$tmp/keyword.ae" <<'AE'
main() {
    when = 0.0
    println("${when}")
}
AE
expect_error "$tmp/keyword.ae" "'when' is a reserved keyword and cannot be used as an identifier" "when = 0.0 does not name the keyword"
expect_error "$tmp/keyword.ae" "keyword.ae:2:5" "the keyword error does not point at the keyword"

# 2b. the keyword error recovers without eating a `}` on the same line, and a
#     user definition of a builtin codegen does NOT intercept stays legal
cat > "$tmp/brace.ae" <<'AE'
helper() -> int { return 1 }
main() {
    x = 1
    if x > 0 { message = 2 }
    println("${helper()}")
}
AE
out="$(AETHER_HOME="$ROOT" "$AETHERC" "$tmp/brace.ae" "$tmp/out.c" 2>&1)"
n="$(printf '%s\n' "$out" | grep -c '^error')"
if [ "$n" -ne 1 ] || ! printf '%s' "$out" | grep -q "'message' is a reserved keyword"; then
    echo "  [FAIL] reserved_name_diagnostics: expected exactly one error (the keyword) for a keyword inside braces, got $n"
    printf '%s\n' "$out" | grep -E "^error" | head -4 | sed 's/^/        /'
    fail=1
fi
cat > "$tmp/each.ae" <<'AE'
import std.list
each(l: ptr, f: fn) {
    i = 0
    while i < list.size(l) {
        v, _ = list.get(l, i)
        call(f, v)
        i = i + 1
    }
}
main() {
    l = list.new()
    list.add(l, "x")
    each(l, |s: string| { println("saw ${s}") })
}
AE
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/each.ae" 2>&1 | tail -1)"
if [ "$got" != "saw x" ]; then
    echo "  [FAIL] reserved_name_diagnostics: a user-defined each (not lowered by name) was refused or misrouted (got '$got')"
    fail=1
fi
# A definition outside the arity codegen intercepts is a plain function:
# `isolate` is lowered by name only with one argument.
cat > "$tmp/isolate2.ae" <<'AE'
isolate(a: int, b: int) -> int { return a + b }
main() { println("${isolate(2, 3)}") }
AE
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/isolate2.ae" 2>&1 | tail -1)"
if [ "$got" != "5" ]; then
    echo "  [FAIL] reserved_name_diagnostics: a two-argument isolate (outside the builtin's arity) was refused (got '$got')"
    fail=1
fi

# 3. `;` after a block
cat > "$tmp/semi.ae" <<'AE'
main() {
    x = 1
    if x > 0 { println("a") } ; println("b")
}
AE
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/semi.ae" 2>&1 | tr '\n' ' ')"
case "$got" in
    *"a b "*) ;;
    *) echo "  [FAIL] reserved_name_diagnostics: a ; after a block is still refused (got '$got')"; fail=1 ;;
esac

# 4. closure into a typed fn pointer
cat > "$tmp/closure.ae" <<'AE'
run(cb: fn(int) -> int) -> int { return cb(3) }
plus(n: int) -> int { return n + 1 }
main() {
    f = |n: int| { return n + 1 }
    println("${run(f)}")
    println("${run(|n: int| { return n * 2 })}")
    println("${run(plus)}")
}
AE
out="$(AETHER_HOME="$ROOT" "$AETHERC" "$tmp/closure.ae" "$tmp/out.c" 2>&1)"
n="$(printf '%s\n' "$out" | grep -c "a closure cannot be passed as a typed function pointer")"
if [ "$n" -ne 2 ]; then
    echo "  [FAIL] reserved_name_diagnostics: expected 2 closure-to-fn-pointer refusals, got $n"
    printf '%s\n' "$out" | grep -E "^error" | head -4 | sed 's/^/        /'
    fail=1
fi
if printf '%s' "$out" | grep -q "closure.ae:7:"; then
    echo "  [FAIL] reserved_name_diagnostics: the named function on line 7 was refused too"
    fail=1
fi

# 5. an error inside a module names the module
mkdir -p "$tmp/lib/noise"
cat > "$tmp/lib/noise/module.ae" <<'AE'
exports(frac)

frac(x: float) -> float {
    return x - (floor(x) as int)
}
AE
cat > "$tmp/main.ae" <<'AE'
import noise
main() { println("${noise.frac(2.75)}") }
AE
out="$(cd "$tmp" && AETHER_HOME="$ROOT" "$AETHERC" main.ae "$tmp/out.c" 2>&1)"
if ! printf '%s' "$out" | grep -q "Undefined function 'floor'"; then
    echo "  [FAIL] reserved_name_diagnostics: the module's undefined floor was not reported"
    printf '%s\n' "$out" | head -4 | sed 's/^/        /'
    fail=1
elif ! printf '%s' "$out" | grep -q "noise/module.ae:4:"; then
    echo "  [FAIL] reserved_name_diagnostics: the module error does not name the module file"
    printf '%s\n' "$out" | grep -- "-->" | head -2 | sed 's/^/        /'
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] reserved_name_diagnostics: builtin redefinition, keyword target, ; after a block, closure into fn pointer, module attribution"
fi
exit $fail
