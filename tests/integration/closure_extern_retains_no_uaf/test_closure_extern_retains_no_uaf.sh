#!/bin/sh
# Regression: closure env freed when passed to an `extern` callee
# that retains the closure (heap-use-after-free).
#
# The probe registers a capturing closure with a C-side sink that
# stores it, then triggers the C side to invoke it. An extern lends its
# arguments unless the parameter is declared `@retain` (R4): the sink's
# is, so the call hands it a reference of its own and the caller's
# release after the call leaves the closure alive for the later call.
#
# We run the probe under ASan (whose `heap-use-after-free` report
# is the canonical detector for this shape). It must print
# "captured" and exit 0 with no ASan diagnostics.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cd "$SCRIPT_DIR" || exit 1

build_log="$tmpdir/build.log"
# Build with ASan so the UAF (if it ever regresses) surfaces as a
# clear `heap-use-after-free` diagnostic rather than a stack-format
# guess.
# AETHER_CFLAGS_EXTRA / AETHER_LDFLAGS_EXTRA reach ae's compile and link
# (they were never read before, so this built uninstrumented). A toolchain
# without ASan (MinGW gcc) cannot link it: the probe then runs plain, which
# still catches a crash, though not a quiet use-after-free.
if ! AETHER_CFLAGS_EXTRA="-fsanitize=address -g" AETHER_LDFLAGS_EXTRA="-fsanitize=address" \
       "$AE" build probe.ae -o "$tmpdir/probe" >"$build_log" 2>&1 &&
   ! "$AE" build probe.ae -o "$tmpdir/probe" >"$build_log" 2>&1; then
    echo "  [FAIL] closure_extern_retains_no_uaf: ae build failed"
    sed 's/^/    /' "$build_log" | head -30
    exit 1
fi

run_log="$tmpdir/run.log"
ASAN_OPTIONS=abort_on_error=0:detect_leaks=0 "$tmpdir/probe" >"$run_log" 2>&1
rc=$?

if [ $rc -ne 0 ]; then
    echo "  [FAIL] closure_extern_retains_no_uaf: probe exited $rc (expect 0)"
    sed 's/^/    /' "$run_log" | head -30
    exit 1
fi

if grep -q "AddressSanitizer" "$run_log"; then
    echo "  [FAIL] closure_extern_retains_no_uaf: ASan diagnostic surfaced"
    sed 's/^/    /' "$run_log" | head -30
    exit 1
fi

if ! grep -q "^captured$" "$run_log"; then
    echo "  [FAIL] closure_extern_retains_no_uaf: probe did not print 'captured'"
    sed 's/^/    /' "$run_log" | head -10
    exit 1
fi

echo "  [PASS] closure_extern_retains_no_uaf: extern-retained closure invokes cleanly"
exit 0
