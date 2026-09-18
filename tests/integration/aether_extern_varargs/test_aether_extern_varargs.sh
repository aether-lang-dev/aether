#!/bin/sh
# Regression: extern `... ` varargs forwarding.
#
# v1 surface — extern decls only:
#     extern printf(fmt: string, ...) -> int
#     extern bare_va(...) -> int
#
# Lowered to C `(const char *, ...)` and `(...)` respectively;
# call sites pass any number of trailing args literally.  No Aether
# function-DEFINING varargs in v1 (no va_list consumption Aether-side).

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AETHERC" ] || [ ! -x "$AE" ]; then
    echo "  [SKIP] aether_extern_varargs: toolchain not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir" || true' EXIT

# Case 1: extern with named param + `...` after a comma.
cat > "$tmpdir/case1.ae" <<'AE'
extern my_format(fmt: string, ...) -> int

main() {
    my_format("answer = %d, name = %s\n", 42, "alice")
    my_format("no extra args\n")
    my_format("%d + %d = %d\n", 2, 3, 5)
}
AE

if ! "$AETHERC" "$tmpdir/case1.ae" "$tmpdir/case1.gen.c" 2>"$tmpdir/case1.err"; then
    echo "  [FAIL] case 1 (named + ...): aetherc returned non-zero"
    cat "$tmpdir/case1.err" | sed 's/^/    /'
    exit 1
fi

# The C prototype must include the `...` after the named param.
if ! grep -qE 'int my_format\(const char\*,\s*\.\.\.\)' "$tmpdir/case1.gen.c"; then
    echo "  [FAIL] case 1: extern prototype missing '...'"
    grep my_format "$tmpdir/case1.gen.c" | sed 's/^/    /'
    exit 1
fi

# Case 2: bare `(...)` — no named params.
cat > "$tmpdir/case2.ae" <<'AE'
extern bare_va(...) -> int

main() {
    bare_va(1, 2, 3)
}
AE

if ! "$AETHERC" "$tmpdir/case2.ae" "$tmpdir/case2.gen.c" 2>"$tmpdir/case2.err"; then
    echo "  [FAIL] case 2 (bare ...): aetherc returned non-zero"
    cat "$tmpdir/case2.err" | sed 's/^/    /'
    exit 1
fi

if ! grep -qE 'int bare_va\(\.\.\.\)' "$tmpdir/case2.gen.c"; then
    echo "  [FAIL] case 2: extern prototype missing bare '...'"
    grep bare_va "$tmpdir/case2.gen.c" | sed 's/^/    /'
    exit 1
fi

# Case 3: end-to-end — declare `extern printf(fmt: string, ...) -> int`,
# call with mixed argument types, run the program, check stdout.
cat > "$tmpdir/case3.ae" <<'AE'
extern printf(fmt: string, ...) -> int

main() {
    printf("answer = %d, name = %s\n", 42, "alice")
    printf("no extra args\n")
}
AE

if ! AETHER_HOME="$ROOT" "$AE" run "$tmpdir/case3.ae" >"$tmpdir/case3.out" 2>"$tmpdir/case3.err"; then
    echo "  [FAIL] case 3 (e2e printf): ae run returned non-zero"
    cat "$tmpdir/case3.err" | sed 's/^/    /'
    exit 1
fi

expected="answer = 42, name = alice
no extra args"
got="$(cat "$tmpdir/case3.out")"

if [ "$got" != "$expected" ]; then
    echo "  [FAIL] case 3 e2e output mismatch"
    echo "  expected:" ; echo "$expected" | sed 's/^/    /'
    echo "  got:"      ; echo "$got"      | sed 's/^/    /'
    exit 1
fi

# Case 4: non-varargs extern unchanged — sanity that the parser
# didn't break the bare case.
cat > "$tmpdir/case4.ae" <<'AE'
extern fixed(a: int, b: int) -> int

main() {
    fixed(1, 2)
}
AE

if ! "$AETHERC" "$tmpdir/case4.ae" "$tmpdir/case4.gen.c" 2>"$tmpdir/case4.err"; then
    echo "  [FAIL] case 4 (non-varargs sanity): aetherc returned non-zero"
    cat "$tmpdir/case4.err" | sed 's/^/    /'
    exit 1
fi

if grep -qE '\.\.\.' "$tmpdir/case4.gen.c"; then
    # Should NOT contain `...` for a non-varargs extern.
    if grep -qE 'fixed.*\.\.\.' "$tmpdir/case4.gen.c"; then
        echo "  [FAIL] case 4: '...' wrongly emitted for non-varargs extern"
        grep fixed "$tmpdir/case4.gen.c" | sed 's/^/    /'
        exit 1
    fi
fi

echo "  [PASS] aether_extern_varargs"
exit 0
