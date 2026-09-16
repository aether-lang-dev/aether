#!/bin/sh
# Regression: `ae build --emit=lib --target=x86_64-freebsd` must link a SHARED
# LIBRARY, not an executable — so it must NOT require a `main`.
#
# Two regressions in the FreeBSD --emit=lib cross path, both in tools/ae_cross.c:
#
# 1. The FreeBSD cross-link branch was written for the exe path and never passed
#    `-shared -fPIC` for --emit=lib (the Tier-A linux/macos/windows branch did;
#    FreeBSD, which shipped no library until selaenium's cross build, fell through
#    without them). Without -shared the link pulls crt1.o and
#    `ld.lld: error: undefined symbol: main`, and no .so is produced.
#
# 2. Even with -shared on the LINK, the runtime OBJECTS that go into libaether.a
#    were compiled without -fPIC, so a FreeBSD 15 target's .o's carried absolute
#    (R_X86_64_32/32S/PC32) and initial-exec TLS (R_X86_64_TPOFF32) relocations
#    that ld.lld rejects in a .so — a wall of "recompile with -fPIC". -fPIC now
#    goes on the object compiles too (and switches the TLS model to a dynamic one
#    the AETHER_TLS_SHARED annotation asks for). linux/macos default to PIC on
#    their zig targets so never hit either.
# (selaenium FreeBSD release leg, ae 0.678→0.679; asks/aether-freebsd-crossbuild-*.)
#
# Needs the FreeBSD base sysroot (AETHER_SYSROOT) that a Tier-B target requires,
# so it SKIPS unless AETHER_SYSROOT is set and holds a base libc — normal CI has
# neither zig-bundled FreeBSD libc nor the base, so this runs on a provisioned
# crossbuild host (aether-crossbuild) or a dev box with the sysroot staged.
#
# The lib `import std.os` pulls the reloc-bearing runtime objects into the link
# (a trivial arithmetic lib would let the linker GC them and hide regression 2).
#
# Asserts: the build succeeds, produces an ELF shared object (not an executable),
# and the output shows neither the undefined-`main` nor the "recompile with
# -fPIC" reloc errors.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if ! command -v zig >/dev/null 2>&1; then
    echo "  [PASS] cross_freebsd_emit_lib_no_main: SKIP (zig not installed)"
    exit 0
fi
if [ -z "$AETHER_SYSROOT" ]; then
    echo "  [PASS] cross_freebsd_emit_lib_no_main: SKIP (AETHER_SYSROOT unset — needs a FreeBSD base sysroot)"
    exit 0
fi
# A base sysroot has libc; a deps-only sysroot does not — skip rather than fail
# the same way the toolchain's own preflight would reject it.
if [ ! -f "$AETHER_SYSROOT/usr/lib/libc.a" ] && [ ! -f "$AETHER_SYSROOT/lib/libc.a" ]; then
    echo "  [PASS] cross_freebsd_emit_lib_no_main: SKIP (AETHER_SYSROOT has no FreeBSD base libc)"
    exit 0
fi

TMPDIR_T="$(mktemp -d)"
cleanup() { rm -rf "$TMPDIR_T"; }
trap cleanup EXIT INT TERM

out="$TMPDIR_T/libprobe.so"
log="$TMPDIR_T/build.log"
# --with=os: lib.ae imports std.os, whose capability the --emit=lib ABI gate
# requires be granted explicitly (and which pulls the reloc-bearing runtime).
if ! (cd "$SCRIPT_DIR" && "$AE" build --emit=lib --with=os --target=x86_64-freebsd lib.ae -o "$out") > "$log" 2>&1; then
    echo "  [FAIL] cross_freebsd_emit_lib_no_main: freebsd --emit=lib did not build"
    sed 's/^/    /' "$log" | head -20
    exit 1
fi

if grep -qi "undefined symbol: main" "$log"; then
    echo "  [FAIL] cross_freebsd_emit_lib_no_main: link demanded 'main' (missing -shared -fPIC on the link)"
    grep -i "undefined symbol: main" "$log" | sed 's/^/    /' | head -2
    exit 1
fi

if grep -qiE "recompile with -fPIC|can not be used against|cannot be used against" "$log"; then
    echo "  [FAIL] cross_freebsd_emit_lib_no_main: absolute/TLS relocations in a .so (runtime objects not built -fPIC)"
    grep -iE "R_X86_64|recompile with -fPIC|TPOFF" "$log" | sed 's/^/    /' | head -4
    exit 1
fi

if [ ! -f "$out" ]; then
    echo "  [FAIL] cross_freebsd_emit_lib_no_main: no shared object produced"
    exit 1
fi

# It must be a shared object, not an executable. `file` reports "shared object"
# for a .so and "executable" for an exe; grep for the former.
kind="$(file "$out" 2>/dev/null)"
case "$kind" in
    *"shared object"*)
        echo "  [PASS] freebsd --emit=lib links a shared object with no main"
        echo "PASS: cross_freebsd_emit_lib_no_main"
        exit 0
        ;;
    *)
        echo "  [FAIL] cross_freebsd_emit_lib_no_main: output is not a shared object:"
        echo "         $kind"
        exit 1
        ;;
esac
