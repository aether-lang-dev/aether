#!/bin/sh
# Regression Part 2 for closure-env-freed-when-passed-to-extern-callee.
#
# Shape under test: cross-module visible-body wrapper whose first param
# is `_ctx: ptr`. The caller omits _ctx (codegen auto-injects
# `_aether_ctx_get()` at AST arg 0), so the closure arg's AST position
# is one less than its position in the wrapper's declared params.
#
# Pre-Part-2-fix, the call-site drain gate at codegen_stmt.c (~line
# 5736) consulted `callee_param_escapes_via_body(callee, AST_arg_idx)`
# without mapping the AST arg index to the function-def param index.
# It asked about `label` (provably non-escaping) instead of `on_press`
# (escapes via `box_closure(on_press) → extern store`), got a
# false-non-escape verdict, and emitted `free(_ad_0.env)` after the
# call. The C side later invoked the closure → read freed env → UAF.
#
# This shape is exactly aether-ui's calculator buttons (`btn("7")
# callback { call(digit, 7) }` with `btn(_ctx, label, on_press)`).
# We run under ASan; passes mean no `_ad_X.env` free is emitted and
# the C-side invocation succeeds cleanly.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cd "$SCRIPT_DIR" || exit 1

build_log="$tmpdir/build.log"
# AETHER_CFLAGS_EXTRA / AETHER_LDFLAGS_EXTRA reach ae's compile and link
# (they were never read before, so this built uninstrumented). A toolchain
# without ASan (MinGW gcc) cannot link it: the probe then runs plain, which
# still catches a crash, though not a quiet use-after-free.
if ! AETHER_CFLAGS_EXTRA="-fsanitize=address -g" AETHER_LDFLAGS_EXTRA="-fsanitize=address" \
       "$AE" build probe.ae -o "$tmpdir/probe" >"$build_log" 2>&1 &&
   ! "$AE" build probe.ae -o "$tmpdir/probe" >"$build_log" 2>&1; then
    echo "  [FAIL] closure_builder_ctx_inject_no_uaf: ae build failed"
    sed 's/^/    /' "$build_log" | head -30
    exit 1
fi

run_log="$tmpdir/run.log"
ASAN_OPTIONS=abort_on_error=0:detect_leaks=0 "$tmpdir/probe" >"$run_log" 2>&1
rc=$?

if [ $rc -ne 0 ]; then
    echo "  [FAIL] closure_builder_ctx_inject_no_uaf: probe exited $rc (expect 0)"
    sed 's/^/    /' "$run_log" | head -30
    exit 1
fi

if grep -q "AddressSanitizer" "$run_log"; then
    echo "  [FAIL] closure_builder_ctx_inject_no_uaf: ASan diagnostic surfaced"
    sed 's/^/    /' "$run_log" | head -30
    exit 1
fi

if ! grep -q "^ok$" "$run_log"; then
    echo "  [FAIL] closure_builder_ctx_inject_no_uaf: probe did not print 'ok'"
    sed 's/^/    /' "$run_log" | head -10
    exit 1
fi

echo "  [PASS] closure_builder_ctx_inject_no_uaf: extern-retained closure invokes cleanly under _ctx-injection"
exit 0
