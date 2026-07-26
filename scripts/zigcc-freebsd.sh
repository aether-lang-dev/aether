#!/bin/sh
# zigcc-freebsd.sh — a `cc` shim for cross-building the Aether toolchain FOR
# FreeBSD with `zig cc` (used by the Makefile's FREEBSD=1 mode).
#
# Why this exists: on a FreeBSD `-nostdlib` link, `zig cc` prints a COSMETIC
# "error: libc not available" and exits nonzero even though clang+lld produced
# a perfectly good binary — zig reserves that message for targets whose libc it
# can't supply, which is exactly why we bring our own base sysroot. A genuine
# link failure (undefined symbol, etc.) leaves NO output file, so "the -o
# target exists and is non-empty" cleanly separates cosmetic from real. This
# mirrors tools/ae_cross.c's handling for `ae build --target=x86_64-freebsd`.
#
# Usage (from the Makefile):
#   CC="scripts/zigcc-freebsd.sh <zig> -target x86_64-freebsd --sysroot=..."
# i.e. $1 is the zig binary, the rest are normal cc args. Compile steps (no
# produced executable to check, plain -c) pass their exit code through
# unchanged, so real compile errors still fail the build.

set -eu

ZIG="$1"
shift

# Find the -o output path (if any), and whether this is a compile-only step.
# Only LINK steps get the cosmetic-failure forgiveness; a `-c` compile must
# always propagate its exit code so real compile errors fail the build.
out=""
prev=""
compile_only=0
for a in "$@"; do
    if [ "$prev" = "-o" ]; then out="$a"; fi
    if [ "$a" = "-c" ]; then compile_only=1; fi
    prev="$a"
done

# Run the real zig cc. Don't let `set -e` abort — we inspect the code.
set +e
"$ZIG" cc "$@"
rc=$?
set -e

if [ "$rc" -eq 0 ]; then
    exit 0
fi

# Nonzero on a LINK step: forgive the cosmetic "libc not available" case, and
# ONLY when the intended output was actually produced (non-empty). A genuine
# link failure (undefined symbol) leaves no output, so this cleanly separates
# cosmetic from real. Compile steps and outputless invocations always fail.
if [ "$compile_only" -eq 0 ] && [ -n "$out" ] && [ -s "$out" ]; then
    echo "note: zig cc exited $rc (cosmetic 'libc not available'); '$out' produced, treating the link as success." >&2
    exit 0
fi

exit "$rc"
