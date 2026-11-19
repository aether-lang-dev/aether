#!/bin/sh
# #3622: a C bridge builds an Aether closure with the runtime's public
# constructor (include/aether_closure.h) instead of copying the box and env
# layout codegen uses, which broke such bridges silently when it changed.
#
# bridge.c makes closures with aether_closure_new and shares and gives them
# back with aether_closure_retain and aether_closure_release; probe.ae opens
# them with unbox_closure, calls them, captures one in an Aether closure and
# gives its boxes back with release_closure. The probe checks that the
# function is handed the env it was made with, that `release` runs exactly
# once and only when the last reference (C's, Aether's or a capturing
# closure's) goes, with that env, and that a round leaks nothing. It must
# print "ok" and exit 0.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] closure_from_c: toolchain not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cd "$SCRIPT_DIR" || exit 1

build_log="$tmpdir/build.log"
if ! "$AE" build probe.ae -o "$tmpdir/probe" >"$build_log" 2>&1; then
    echo "  [FAIL] closure_from_c: ae build failed"
    sed 's/^/    /' "$build_log" | head -30
    exit 1
fi

run_log="$tmpdir/run.log"
"$tmpdir/probe" >"$run_log" 2>&1
rc=$?

if [ $rc -ne 0 ]; then
    echo "  [FAIL] closure_from_c: probe exited $rc (expect 0)"
    sed 's/^/    /' "$run_log" | head -30
    exit 1
fi

if ! tr -d '\r' < "$run_log" | grep -q "^ok$"; then
    echo "  [FAIL] closure_from_c: probe did not print 'ok'"
    sed 's/^/    /' "$run_log" | head -10
    exit 1
fi

echo "  [PASS] closure_from_c: a closure built in C is called and released like one Aether made"
exit 0
