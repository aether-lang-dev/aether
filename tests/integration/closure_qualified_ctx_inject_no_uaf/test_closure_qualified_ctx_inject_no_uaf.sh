#!/bin/sh
# Regression Part 3 for closure-env-freed-when-passed-to-extern-callee.
#
# Shape under test: same as Part 2 (cross-module visible-body wrapper
# with `_ctx: ptr`, _ctx-injection at the call site, closure-captures-
# closure payload) BUT invoked through a whole-module import using
# qualified call syntax (`uikit.register_btn(...)`). This is the form
# aether-ui uses (`import aether_ui` + `aether_ui.btn(...)`).
#
# Pre-Part-3-fix, the drain-gate's _ctx-injection arg→param shift
# looked the callee up in the merged program AST using the RAW
# qualified value (e.g. "uikit.register_btn"). The merged AST stores
# cross-module functions under their normalised (dot→underscore)
# name ("uikit_register_btn"), so the lookup missed, the shift was
# skipped, and the escape walk asked about `label` instead of
# `on_press` → false-non-escape → UAF (exactly aether-ui's
# example_testable.ae `+1` button).
#
# Part 2's selective-import test (closure_builder_ctx_inject_no_uaf)
# passed without this fix because selective imports lower the call
# site to a bare unqualified name, bypassing the missed normalisation.
# This test exists to lock in the qualified-call path.
#
# We run under ASan; pass means no `_ad_X.env` free is emitted and
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
    echo "  [FAIL] closure_qualified_ctx_inject_no_uaf: ae build failed"
    sed 's/^/    /' "$build_log" | head -30
    exit 1
fi

run_log="$tmpdir/run.log"
ASAN_OPTIONS=abort_on_error=0:detect_leaks=0 "$tmpdir/probe" >"$run_log" 2>&1
rc=$?

if [ $rc -ne 0 ]; then
    echo "  [FAIL] closure_qualified_ctx_inject_no_uaf: probe exited $rc (expect 0)"
    sed 's/^/    /' "$run_log" | head -30
    exit 1
fi

if grep -q "AddressSanitizer" "$run_log"; then
    echo "  [FAIL] closure_qualified_ctx_inject_no_uaf: ASan diagnostic surfaced"
    sed 's/^/    /' "$run_log" | head -30
    exit 1
fi

if ! grep -q "^ok$" "$run_log"; then
    echo "  [FAIL] closure_qualified_ctx_inject_no_uaf: probe did not print 'ok'"
    sed 's/^/    /' "$run_log" | head -10
    exit 1
fi

echo "  [PASS] closure_qualified_ctx_inject_no_uaf: extern-retained closure invokes cleanly under qualified _ctx-injection"
exit 0
