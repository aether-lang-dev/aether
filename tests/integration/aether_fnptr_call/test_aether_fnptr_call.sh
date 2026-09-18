#!/bin/sh
# Regression: typed C-function-pointer locals.
#
# v1 surface — local variables can be typed `fn(T1, T2, ...) -> R`
# or take that type via an `expr as fn(...) -> R` cast on the
# initializer.  Storage in C stays `void*`; the call site emits a
# typed C function-pointer cast around the stored value:
#
#   fp = get_callback() as fn(int, int) -> int
#   fp(3, 4)                  // emits ((int(*)(int, int))(fp))(3, 4)
#
# Replaces the bespoke `mem.call_fn3_int` per-signature shim layer
# for cases where Aether holds a C function pointer (vtable lookup,
# signal handler table, qsort callback handoff) and needs to invoke
# it with type checking.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] aether_fnptr_call: toolchain not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir" || true' EXIT

# -----------------------------------------------------------------
# Case 1: `as fn(...) -> R` cast on a ptr, then direct invocation.
# Verifies the call site emits the typed cast around the value.
# -----------------------------------------------------------------
cat > "$tmpdir/case1.ae" <<'AE'
extern get_adder() -> ptr

main() {
    add = get_adder() as fn(int, int) -> int
    sum = add(3, 4)
    println("3+4=${sum}")
}
AE

if ! "$AETHERC" "$tmpdir/case1.ae" "$tmpdir/case1.gen.c" 2>"$tmpdir/case1.err"; then
    echo "  [FAIL] case 1 (cast + invoke): aetherc returned non-zero"
    cat "$tmpdir/case1.err" | sed 's/^/    /'
    exit 1
fi

# The cast must appear in the emitted C — `((int(*)(int, int))(add))(3, 4)`.
# Allow flexible whitespace between operators / parens.
if ! grep -qE '\(\(int *\(\*\) *\(int, *int\)\) *\(add\)\) *\(3, *4\)' "$tmpdir/case1.gen.c"; then
    echo "  [FAIL] case 1: expected typed fn-pointer cast at call site, got:"
    grep -E 'add' "$tmpdir/case1.gen.c" | sed 's/^/    /'
    exit 1
fi

# Storage of the fn-pointer local must be `void*` — NOT `_AeClosure`.
# Without this carve-out the typed fn-pointer would be packed into the
# closure (.fn, .env) shape and the cast at the call site wouldn't
# match the C-side function signature.
if ! grep -qE 'void\* *add *=' "$tmpdir/case1.gen.c"; then
    echo "  [FAIL] case 1: fn-pointer local should be declared void*, got:"
    grep -E '\<add\>.*=' "$tmpdir/case1.gen.c" | head -3 | sed 's/^/    /'
    exit 1
fi

# The fnptr path must NOT route through the closure call shape
# (`.fn`, `.env`) — that's for _AeClosure values, not bare void*.
if grep -qE '\<add\.fn\>|\<add\.env\>' "$tmpdir/case1.gen.c"; then
    echo "  [FAIL] case 1: fn-pointer call wrongly routed through closure shape:"
    grep -E '\<add\.(fn|env)\>' "$tmpdir/case1.gen.c" | sed 's/^/    /'
    exit 1
fi

# -----------------------------------------------------------------
# Case 2: same `as fn(...)` shape with a unary signature.  Confirms
# the param-list parser handles single-arg and the cast applies
# uniformly across arities.
# -----------------------------------------------------------------
cat > "$tmpdir/case2.ae" <<'AE'
extern get_doubler() -> ptr

main() {
    dbl = get_doubler() as fn(int) -> int
    twox = dbl(21)
    println("2*21=${twox}")
}
AE

if ! "$AETHERC" "$tmpdir/case2.ae" "$tmpdir/case2.gen.c" 2>"$tmpdir/case2.err"; then
    echo "  [FAIL] case 2 (typed annotation): aetherc returned non-zero"
    cat "$tmpdir/case2.err" | sed 's/^/    /'
    exit 1
fi

if ! grep -qE '\(\(int *\(\*\) *\(int\)\) *\(dbl\)\) *\(21\)' "$tmpdir/case2.gen.c"; then
    echo "  [FAIL] case 2: expected typed cast for unary fn-pointer call"
    grep -E 'dbl' "$tmpdir/case2.gen.c" | sed 's/^/    /'
    exit 1
fi

# -----------------------------------------------------------------
# Case 3: void-returning function pointer (`fn(int)` with no `-> R`).
# Confirms the parser defaults the return type to void when `->` is
# omitted, and the call-site cast emits `((void(*)(int))(fp))(...)`.
# -----------------------------------------------------------------
cat > "$tmpdir/case3.ae" <<'AE'
extern get_logger() -> ptr

main() {
    log_it = get_logger() as fn(int)
    log_it(42)
}
AE

if ! "$AETHERC" "$tmpdir/case3.ae" "$tmpdir/case3.gen.c" 2>"$tmpdir/case3.err"; then
    echo "  [FAIL] case 3 (void return): aetherc returned non-zero"
    cat "$tmpdir/case3.err" | sed 's/^/    /'
    exit 1
fi

if ! grep -qE '\(\(void *\(\*\) *\(int\)\) *\(log_it\)\) *\(42\)' "$tmpdir/case3.gen.c"; then
    echo "  [FAIL] case 3: expected void-returning fn-pointer cast"
    grep -E 'log_it' "$tmpdir/case3.gen.c" | sed 's/^/    /'
    exit 1
fi

# -----------------------------------------------------------------
# Case 4: end-to-end execution.  Compile + link with a C glue file
# that defines the underlying functions, run the binary, check the
# output bytes.  This is the proof that the cast machinery
# round-trips real argument and return values through the C ABI.
#
# Skipped on Windows MinGW: the final link pulls in libaether's
# `-ldl` dependency which doesn't exist on Windows (no dlopen).
# Cases 1-3 cover the language-level surface (typed casts, signatures,
# void-return); case 4 only adds the real-world linking shape.
# -----------------------------------------------------------------
SKIP_CASE_4=0
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        SKIP_CASE_4=1
        echo "  [SKIP] case 4 (C-interop link) on Windows: -ldl unavailable"
        ;;
esac

if [ "$SKIP_CASE_4" = 0 ]; then
cat > "$tmpdir/case4.ae" <<'AE'
extern get_adder() -> ptr
extern get_doubler() -> ptr

main() {
    add = get_adder() as fn(int, int) -> int
    sum = add(3, 4)
    println("sum=${sum}")

    dbl = get_doubler() as fn(int) -> int
    twox = dbl(21)
    println("twox=${twox}")
}
AE

cat > "$tmpdir/case4_glue.c" <<'C'
static int adder(int a, int b) { return a + b; }
static int doubler(int x) { return x * 2; }
void* get_adder(void)   { return (void*)adder; }
void* get_doubler(void) { return (void*)doubler; }
C

if ! "$AETHERC" "$tmpdir/case4.ae" "$tmpdir/case4.gen.c" 2>"$tmpdir/case4.err"; then
    echo "  [FAIL] case 4 (end-to-end): aetherc returned non-zero"
    cat "$tmpdir/case4.err" | sed 's/^/    /'
    exit 1
fi

# Walk the stdlib include tree to find aether_panic.h and the others.
# Cheap: include every dir under std/ and runtime/.
INCLUDES=""
for d in "$ROOT" "$ROOT/runtime" "$ROOT/runtime/actors" "$ROOT/runtime/scheduler" \
         "$ROOT/runtime/memory" "$ROOT/runtime/utils" "$ROOT/runtime/config" \
         "$ROOT/runtime/simd" "$ROOT/std" "$ROOT/std/string" "$ROOT/std/io" \
         "$ROOT/std/math" "$ROOT/std/collections" "$ROOT/std/mem" \
         "$ROOT/std/bytes" "$ROOT/std/log"; do
    INCLUDES="$INCLUDES -I$d"
done

if ! gcc $INCLUDES -o "$tmpdir/case4_bin" \
        "$tmpdir/case4.gen.c" "$tmpdir/case4_glue.c" \
        "$ROOT/build/libaether.a" -lpthread -ldl -lm 2>"$tmpdir/case4.gcc.err"; then
    echo "  [FAIL] case 4: gcc returned non-zero"
    cat "$tmpdir/case4.gcc.err" | sed 's/^/    /'
    exit 1
fi

if ! "$tmpdir/case4_bin" > "$tmpdir/case4.out" 2>"$tmpdir/case4.runerr"; then
    echo "  [FAIL] case 4: binary returned non-zero"
    cat "$tmpdir/case4.runerr" | sed 's/^/    /'
    exit 1
fi

expected="sum=7
twox=42"
got="$(cat "$tmpdir/case4.out")"

if [ "$got" != "$expected" ]; then
    echo "  [FAIL] case 4: output mismatch"
    echo "  expected:" ; echo "$expected" | sed 's/^/    /'
    echo "  got:"      ; echo "$got"      | sed 's/^/    /'
    exit 1
fi
fi  # SKIP_CASE_4 guard

# -----------------------------------------------------------------
# Case 5: bare `fn` (no signature) still works for closures.
# Pre-feature backward-compatibility check.
# -----------------------------------------------------------------
cat > "$tmpdir/case5.ae" <<'AE'
main() {
    doubler = |x: int| -> x * 2
    r = call(doubler, 21)
    println("r=${r}")
}
AE

if ! "$AETHERC" "$tmpdir/case5.ae" "$tmpdir/case5.gen.c" 2>"$tmpdir/case5.err"; then
    echo "  [FAIL] case 5 (closure regression): aetherc returned non-zero"
    cat "$tmpdir/case5.err" | sed 's/^/    /'
    exit 1
fi

# Closure must still use the `_AeClosure`-shaped local — confirms the
# bare-fn path didn't get poisoned by the new is_fnptr branch.
if ! grep -qE '_AeClosure +doubler' "$tmpdir/case5.gen.c"; then
    echo "  [FAIL] case 5: closure local should still be _AeClosure-typed"
    grep -E 'doubler' "$tmpdir/case5.gen.c" | sed 's/^/    /'
    exit 1
fi

echo "  [PASS] aether_fnptr_call"
exit 0
