#!/bin/sh
# std/lzf: LZF_USE_OFFSETS must pick the 64-bit path on every 64-bit target,
# and must not be written as a macro that expands to `defined(...)`.
#
# Upstream had `#define LZF_USE_OFFSETS defined(_M_X64)` under _WIN32. Two
# faults, and the second is the one that actually bit:
#
#   - `_M_X64` is MSVC-only. clang targeting MinGW -- which is what the cross
#     build uses -- defines neither _M_X64 nor _M_ARM64, so the test was false
#     on x86_64-windows as well as aarch64-windows. Adding _M_ARM64 would not
#     have fixed the cross build.
#   - expanding to `defined(...)` inside a macro is undefined behaviour;
#     clang warns -Wexpansion-to-defined and the standard permits worse.
#
# This asserts the portable UINTPTR_MAX test selects 64-bit offsets on each
# 64-bit target and 32-bit on a 32-bit one, compiling the real header rather
# than a copy of the expression, so drift in lzfP.h is what fails the test.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
HDR="$ROOT/std/lzf/lzfP.h"

[ -f "$HDR" ] || { echo "  [FAIL] lzfP.h not found"; exit 1; }

# The regression in source form: catch a reintroduced `defined(...)` macro
# even on a box with no cross toolchain.
if grep -qE '^[[:space:]]*#[[:space:]]*define[[:space:]]+LZF_USE_OFFSETS[[:space:]]+defined' "$HDR"; then
    echo "  [FAIL] LZF_USE_OFFSETS expands to defined(...) — undefined behaviour"
    exit 1
fi
# Only preprocessor directives count -- the comment above the fix names
# _M_X64 to explain why it went, and that must not trip this.
if grep -E '^[[:space:]]*#' "$HDR" | grep -q '_M_X64'; then
    echo "  [FAIL] lzfP.h still tests _M_X64 (MSVC-only; false under clang/MinGW)"
    exit 1
fi

ZIG="$(command -v zig 2>/dev/null || true)"
[ -n "$ZIG" ] || { echo "  [SKIP] lzf_offsets_portable: no zig for cross probes"; exit 0; }

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP" || :; return 0; }
trap cleanup EXIT

# Compile the REAL header and let the preprocessor report which path it chose.
cat > "$TMP/probe.c" <<'EOF'
#include "lzfP.h"
#if LZF_USE_OFFSETS
#error "SELECTED=offsets64"
#else
#error "SELECTED=pointer32"
#endif
EOF

check() {   # $1=zig triple  $2=expected
    got=$("$ZIG" cc --target="$1" -I "$ROOT/std/lzf" -c "$TMP/probe.c" -o /dev/null 2>&1 \
            | grep -oE 'SELECTED=(offsets64|pointer32)' | head -1)
    if [ "$got" != "SELECTED=$2" ]; then
        echo "  [FAIL] $1: expected SELECTED=$2, got '${got:-<none>}'"
        exit 1
    fi
    # -Wexpansion-to-defined would flag a reintroduced defined(...) macro.
    if "$ZIG" cc --target="$1" -I "$ROOT/std/lzf" -Wexpansion-to-defined \
         -fsyntax-only "$TMP/quiet.c" 2>&1 | grep -q 'expansion-to-defined'; then
        echo "  [FAIL] $1: LZF_USE_OFFSETS triggers -Wexpansion-to-defined"
        exit 1
    fi
}

printf '#include "lzfP.h"\nint lzf_probe_ok;\n' > "$TMP/quiet.c"

check aarch64-windows-gnu offsets64   # the target that prompted this
check x86_64-windows-gnu  offsets64   # was ALSO wrong before the fix
check x86_64-linux-gnu    offsets64
check aarch64-linux-gnu   offsets64
check x86_64-macos        offsets64
check aarch64-macos       offsets64
check x86-windows-gnu     pointer32   # 32-bit must NOT take the offsets path

echo "  [PASS] lzf_offsets_portable: 64-bit offsets on 6 targets, 32-bit path on x86-windows"
