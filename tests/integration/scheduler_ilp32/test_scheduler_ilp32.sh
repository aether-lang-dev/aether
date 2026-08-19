#!/bin/sh
# Regression: aether#1652. multicore_scheduler.c must compile for a 32-bit
# (ILP32) target.
#
# It did not. The Message assertion was guarded by `#if INTPTR_MAX ==
# INT64_MAX`, the Mailbox one directly below it was not:
#
#     _Static_assert(sizeof(Mailbox) % 8 == 0, "Mailbox not 8-byte aligned");
#
# On LP64 that is vacuously true — Mailbox contains pointers, so its size is
# necessarily a multiple of 8 — and on ILP32 it is vacuously false
# (sizeof(Mailbox) == 1036, 1036 % 8 == 4). It asserted a property no
# conforming ABI can violate, and the only thing it ever detected was the
# pointer width, so every 32-bit triple failed to compile the TU.
#
# Nothing in CI links a 32-bit executable: the Cortex-M4 leg builds the
# COOPERATIVE scheduler (PLATFORM=embedded, a different file), --target=wasm
# goes through Emscripten, and every zig triple in the matrix is 64-bit. So
# this compiles the one TU for an ILP32 target directly.
#
# Uses the watchOS SDK's arm64_32 (32-bit pointers, arm64 ISA) because it is a
# real ILP32 target with real headers available wherever Xcode is. The bug is
# not watchOS-specific — any 32-bit triple reproduces it.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

fail() { echo "  FAIL: $1"; exit 1; }

if [ "$(uname -s)" != "Darwin" ]; then
    echo "  [skip] ILP32 scheduler compile: needs macOS + an Xcode SDK"
    exit 0
fi
SDK=$(xcrun --sdk watchos --show-sdk-path 2>/dev/null) || SDK=""
if [ -z "$SDK" ]; then
    echo "  [skip] ILP32 scheduler compile: no watchOS SDK (Xcode not installed)"
    exit 0
fi

TMP="${TMPDIR:-/tmp}/ae_ilp32_$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC=$("$ROOT/build/ae" cflags --cflags)

# 1. The TU compiles for ILP32 at all (this is the #1652 regression).
xcrun --sdk watchos clang -target arm64_32-apple-watchos8.0 -isysroot "$SDK" \
    -O2 -w -c "$ROOT/runtime/scheduler/multicore_scheduler.c" $INC \
    -o "$TMP/sched.o" 2>"$TMP/err" \
    || { cat "$TMP/err"; fail "multicore_scheduler.c does not compile for arm64_32 (ILP32)"; }

# 2. Confirm ILP32 really was in effect — otherwise a toolchain change that
#    silently produced a 64-bit object would make this test vacuous, which is
#    the exact failure mode the assertion it guards already had.
cat > "$TMP/width.c" <<'EOF'
#include <stdint.h>
_Static_assert(INTPTR_MAX == INT32_MAX, "not an ILP32 target");
EOF
xcrun --sdk watchos clang -target arm64_32-apple-watchos8.0 -isysroot "$SDK" \
    -w -c "$TMP/width.c" -o "$TMP/width.o" 2>"$TMP/werr" \
    || { cat "$TMP/werr"; fail "arm64_32 target is not ILP32 — this test proves nothing"; }

echo "  PASS: multicore_scheduler.c compiles for ILP32 (arm64_32), width confirmed"
