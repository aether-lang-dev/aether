#!/bin/sh
# Regression: `--emit=lib --emit-main=<func>` produces a TU with the
# `aether_<name>` library exports AND a thin main(argc,argv) shim that
# calls the named function. The output compiles to both a loadable lib
# and a regular binary from one .c. Issue #268.3.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
LIBAETHER="$ROOT/build/libaether.a"

if [ ! -x "$AETHERC" ] || [ ! -f "$LIBAETHER" ]; then
    echo "  [SKIP] aetherc_emit_main: toolchain not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cat > "$tmpdir/svc.ae" <<'AE'
import std.io

run() -> int {
    println("aetherc_emit_main: shim entered")
    return 7
}

square(n: int) -> int {
    return n * n
}
AE

# Compile with --emit=lib + --emit-main=run.
if ! "$AETHERC" --emit=lib --emit-main=run "$tmpdir/svc.ae" "$tmpdir/svc.c" >"$tmpdir/aetherc.log" 2>&1; then
    echo "  [FAIL] aetherc_emit_main: aetherc rejected the source"
    cat "$tmpdir/aetherc.log"
    exit 1
fi

# Generated C must contain BOTH the library export and the main shim.
if ! grep -q '^int32_t aether_run' "$tmpdir/svc.c"; then
    echo "  [FAIL] aetherc_emit_main: aether_run library export missing"
    exit 1
fi
if ! grep -q '^int32_t aether_square' "$tmpdir/svc.c"; then
    echo "  [FAIL] aetherc_emit_main: aether_square library export missing"
    exit 1
fi
if ! grep -q '^int main(int argc' "$tmpdir/svc.c"; then
    echo "  [FAIL] aetherc_emit_main: main() shim missing"
    exit 1
fi

# Compile to a binary and run it. Should print the message and exit 7.
gcc_args="-O0 -I$ROOT -I$ROOT/runtime -I$ROOT/runtime/actors -I$ROOT/runtime/scheduler -I$ROOT/runtime/utils -I$ROOT/runtime/memory -I$ROOT/runtime/config -I$ROOT/std -I$ROOT/std/string -I$ROOT/std/io -I$ROOT/std/math -I$ROOT/std/net -I$ROOT/std/collections -I$ROOT/std/json"
if ! ${CC:-cc} $gcc_args "$tmpdir/svc.c" "$LIBAETHER" -o "$tmpdir/svc" -pthread -lm >"$tmpdir/gcc.log" 2>&1; then
    echo "  [FAIL] aetherc_emit_main: gcc failed to link the dual-emit output"
    cat "$tmpdir/gcc.log"
    exit 1
fi

actual="$("$tmpdir/svc" 2>&1)"
exit_code=$?
expected="aetherc_emit_main: shim entered"
if [ "$actual" != "$expected" ]; then
    echo "  [FAIL] aetherc_emit_main: output '$actual' (expected '$expected')"
    exit 1
fi
if [ "$exit_code" != "7" ]; then
    echo "  [FAIL] aetherc_emit_main: exit code $exit_code (expected 7 forwarded from run())"
    exit 1
fi

# Reject path: target function doesn't exist.
if "$AETHERC" --emit=lib --emit-main=does_not_exist "$tmpdir/svc.ae" "$tmpdir/x.c" >"$tmpdir/missing.log" 2>&1; then
    echo "  [FAIL] aetherc_emit_main: missing target should be rejected"
    exit 1
fi
if ! grep -q "no top-level function named" "$tmpdir/missing.log"; then
    echo "  [FAIL] aetherc_emit_main: missing-target error didn't mention the missing function"
    cat "$tmpdir/missing.log"
    exit 1
fi

# Reject path: target takes parameters (`square(n: int)` takes one arg).
if "$AETHERC" --emit=lib --emit-main=square "$tmpdir/svc.ae" "$tmpdir/x.c" >"$tmpdir/params.log" 2>&1; then
    echo "  [FAIL] aetherc_emit_main: target with params should be rejected"
    exit 1
fi
if ! grep -q "must be zero-arg" "$tmpdir/params.log"; then
    echo "  [FAIL] aetherc_emit_main: param-count rejection produced an unhelpful error"
    cat "$tmpdir/params.log"
    exit 1
fi

echo "  [PASS] aetherc_emit_main: dual-emit output runs (exit=7), aether_* exports present, error paths clean"
exit 0
