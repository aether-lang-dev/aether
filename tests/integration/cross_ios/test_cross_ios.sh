#!/bin/sh
# `ae build --target=aarch64-ios` and the simulator variants.
#
# iOS is the one cross target NOT served by zig: Apple's SDKs are Xcode-
# licensed and not redistributable, so these triples shell to `xcrun clang`
# with the SDK xcrun reports. That also makes iOS the one cross target where
# --emit=lib is supported, because an iOS app is built by Xcode and what it
# wants from Aether is a loadable library, not a standalone binary (iOS will
# not run a loose executable at all).
#
# Asserts:
#   - aarch64-ios / aarch64-ios-simulator / x86_64-ios-simulator are accepted
#   - the Mach-O PLATFORM distinguishes device (IOS) from simulator
#     (IOSSIMULATOR) — same arch, non-interchangeable binaries
#   - AETHER_IOS_MIN sets the deployment target (LC_BUILD_VERSION minos)
#   - --emit=lib produces a dylib whose install_name is @rpath-relative (an
#     absolute build path there fails to load from inside an .app bundle)
#   - --emit=csrc needs no Xcode at all (it never invokes the toolchain)
#
# Skips off macOS, or when Xcode's iPhoneOS SDK is absent (the Command Line
# Tools alone do not carry one).
#
# NB on cost: an executable/dylib build recompiles the whole runtime and
# stdlib for the target (~90 TUs, no per-target archive cache yet), so this
# does exactly TWO of those. Everything that can be asserted on a single-TU
# --emit=obj is, because that path is ~100x cheaper and carries the same
# LC_BUILD_VERSION the linked artifacts do.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
SRC="$SCRIPT_DIR/mylib.ae"      # exports, no main() — lib/obj/csrc
EXE_SRC="$SCRIPT_DIR/hello.ae"  # has main() — executable build

TMP="${TMPDIR:-/tmp}/ae_cross_ios_$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

fail() { echo "  FAIL: $1"; exit 1; }

# --- csrc first: it must work with no Xcode, so it runs before the skip ---
"$AE" build --target=aarch64-ios --emit=csrc "$SRC" -o "$TMP/csrc" >/dev/null 2>&1 \
    || fail "--target=aarch64-ios --emit=csrc was rejected"
[ -f "$TMP/csrc.c" ] && [ -f "$TMP/csrc.h" ] \
    || fail "--emit=csrc under an iOS target emitted no .c/.h"

if [ "$(uname -s)" != "Darwin" ]; then
    echo "  [skip] iOS cross checks: not a macOS host"
    echo "  PASS: --emit=csrc under an iOS target (rest skipped)"
    exit 0
fi
if ! xcrun --sdk iphoneos --show-sdk-path >/dev/null 2>&1; then
    echo "  [skip] iOS cross checks: no iPhoneOS SDK (Xcode not installed)"
    echo "  PASS: --emit=csrc under an iOS target (rest skipped)"
    exit 0
fi

macho_platform() { vtool -show-build "$1" 2>/dev/null | awk '/platform/ {print $2; exit}'; }
macho_minos()    { vtool -show-build "$1" 2>/dev/null | awk '/minos/ {print $2; exit}'; }

# --- 1. target matrix, on the cheap --emit=obj path ---
build_obj() { # <target> <out> [env-assignment]
    if [ -n "$3" ]; then
        env "$3" "$AE" build --target="$1" --emit=obj "$SRC" -o "$2" >"$TMP/log" 2>&1 \
            || { cat "$TMP/log"; fail "--target=$1 --emit=obj ($3) failed"; }
    else
        "$AE" build --target="$1" --emit=obj "$SRC" -o "$2" >"$TMP/log" 2>&1 \
            || { cat "$TMP/log"; fail "--target=$1 --emit=obj failed"; }
    fi
}

build_obj aarch64-ios            "$TMP/dev.o"
build_obj aarch64-ios-simulator  "$TMP/sim.o"
build_obj x86_64-ios-simulator   "$TMP/sim64.o"
build_obj aarch64-ios            "$TMP/min.o" AETHER_IOS_MIN=17.0

file "$TMP/dev.o"   | grep -q 'Mach-O 64-bit object arm64' \
    || fail "device object is not arm64 Mach-O: $(file "$TMP/dev.o")"
file "$TMP/sim64.o" | grep -q 'Mach-O 64-bit object x86_64' \
    || fail "x86_64 simulator object is not x86_64 Mach-O: $(file "$TMP/sim64.o")"

# Device and simulator are the same ARCH but different PLATFORM; that is the
# whole reason they are separate targets rather than one with a flag.
[ "$(macho_platform "$TMP/dev.o")" = "IOS" ] \
    || fail "device object platform is '$(macho_platform "$TMP/dev.o")', expected IOS"
[ "$(macho_platform "$TMP/sim.o")" = "IOSSIMULATOR" ] \
    || fail "simulator object platform is '$(macho_platform "$TMP/sim.o")', expected IOSSIMULATOR"

[ "$(macho_minos "$TMP/dev.o")" = "15.0" ] \
    || fail "default minos is '$(macho_minos "$TMP/dev.o")', expected 15.0"
[ "$(macho_minos "$TMP/min.o")" = "17.0" ] \
    || fail "AETHER_IOS_MIN=17.0 gave minos '$(macho_minos "$TMP/min.o")'"

# --- 2. a real executable link (full runtime compile) ---
"$AE" build --target=aarch64-ios "$EXE_SRC" -o "$TMP/dev" >"$TMP/log" 2>&1 \
    || { cat "$TMP/log"; fail "--target=aarch64-ios executable build failed"; }
file "$TMP/dev" | grep -q 'Mach-O 64-bit executable arm64' \
    || fail "device build is not a Mach-O arm64 executable: $(file "$TMP/dev")"
[ "$(macho_platform "$TMP/dev")" = "IOS" ] \
    || fail "device exe platform is '$(macho_platform "$TMP/dev")', expected IOS"

# --- 3. --emit=lib is SUPPORTED on iOS (unlike the zig cross targets) ---
"$AE" build --target=aarch64-ios --emit=lib "$SRC" -o "$TMP/libmylib.dylib" >"$TMP/log" 2>&1 \
    || { cat "$TMP/log"; fail "--target=aarch64-ios --emit=lib was rejected"; }
file "$TMP/libmylib.dylib" | grep -q 'Mach-O 64-bit dynamically linked shared library arm64' \
    || fail "--emit=lib did not produce an arm64 dylib: $(file "$TMP/libmylib.dylib")"
# An absolute install_name here is the classic "works on my Mac, fails in the
# .app" bug: the loader would look for the dylib at its BUILD path.
otool -D "$TMP/libmylib.dylib" | tail -1 | grep -q '^@rpath/' \
    || fail "dylib install_name is not @rpath-relative: $(otool -D "$TMP/libmylib.dylib" | tail -1)"
nm -gU "$TMP/libmylib.dylib" | grep -q '_aether_add' \
    || fail "dylib does not export _aether_add"

echo "  PASS: iOS device/simulator objects, arm64 exe, @rpath dylib, csrc; AETHER_IOS_MIN honoured"
