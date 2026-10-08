#!/bin/sh
# Regression (#2476): on Windows, a build with AVX enabled must not fault on
# an aligned 256-bit move to a 16-byte-aligned stack slot.
#
# Win64 only guarantees 16-byte stack alignment and GCC does not realign the
# stack for 32-byte values (GCC bug 54412), yet GCC 15 spilled 256-bit
# temporaries with vmovaps / vmovdqa: an f32x8 solver built with -mavx2
# segfaulted on `vmovdqa %ymm0,0x20(%rsp)`. `ae` now passes
# -Wa,-muse-unaligned-vector-move when the cflags enable AVX, so the
# assembler encodes every aligned vector move as its unaligned twin.
#
# Which moves GCC picks for a given kernel depends on its version, so the
# check goes through the assembler directly: a C file built into the program
# writes a ymm register with an explicit `vmovaps` to an address that is 16
# but not 32 aligned, exactly the faulting store. With the flag it is
# assembled as vmovups and the program runs; without it, it faults.
#
# Also checked: a build without AVX (no -m option, or -mavx2 undone by a
# later -mno-avx) does not get the flag, and no other platform gets it.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] win_avx_unaligned_vector_move: ae not built"
    exit 0
fi
case "$(uname -m 2>/dev/null)" in
    x86_64|amd64) ;;
    *) echo "  [SKIP] win_avx_unaligned_vector_move: x86-64 only"; exit 0 ;;
esac

WIN=0
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) WIN=1 ;;
esac

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
AETHER_CACHE_DIR="$TMP/cache"
export AETHER_CACHE_DIR
FLAG="-Wa,-muse-unaligned-vector-move"

# project DIR CFLAGS: a program whose shim does the faulting store when AVX
# is available at run time.
project() {
    mkdir -p "$1"
    cat > "$1/shim.c" <<'COF'
#include <stdint.h>

/* 1 after storing a ymm register to a 16-but-not-32-aligned address with
 * vmovaps, -1 when this CPU has no AVX (nothing is run). */
int ae_misaligned_ymm_store(void) {
    if (!__builtin_cpu_supports("avx")) return -1;
    static float buf[24];
    uintptr_t a = ((uintptr_t)buf + 31) & ~(uintptr_t)31;
    float* p = (float*)(a + 16);
    __asm__ volatile("vxorps %%ymm0, %%ymm0, %%ymm0\n\t"
                     "vmovaps %%ymm0, (%0)\n\t"
                     "vzeroupper"
                     : : "r"(p) : "xmm0", "memory");
    return 1;
}
COF
    cat > "$1/main.ae" <<'AEOF'
extern ae_misaligned_ymm_store() -> int

main() {
    println("store=${ae_misaligned_ymm_store()}")
}
AEOF
    printf '[[bin]]\nname = "app"\npath = "main.ae"\nextra_sources = ["shim.c"]\n\n[build]\ncflags = "%s"\n' "$2" > "$1/aether.toml"
}

# build DIR: build verbosely, leaving the log in DIR/build.log.
build() {
    if ! (cd "$1" && "$AE" build -v main.ae -o ./app >build.log 2>&1); then
        echo "  [FAIL] win_avx_unaligned_vector_move: build failed in $(basename "$1")"
        grep -v '^\[' "$1/build.log" | sed 's/^/        /' | head -10
        exit 1
    fi
}

# The link of the program itself, not the probes ae runs before it.
has_flag() {
    grep '^\[cmd\]' "$1/build.log" | grep -- '-o "\./app' | grep -q -- "$FLAG"
}

project "$TMP/avx" "-O2 -mavx2"
build "$TMP/avx"
if [ "$WIN" = 1 ]; then
    if grep -q "does not take -muse-unaligned-vector-move" "$TMP/avx/build.log"; then
        echo "  [SKIP] win_avx_unaligned_vector_move: this assembler predates binutils 2.38"
        exit 0
    fi
    if ! has_flag "$TMP/avx"; then
        echo "  [FAIL] win_avx_unaligned_vector_move: an -mavx2 build on Windows did not pass $FLAG"
        grep '^\[cmd\]' "$TMP/avx/build.log" | grep -- '-o "\./app' | cut -c1-300 | sed 's/^/        /'
        exit 1
    fi
    got=$(cd "$TMP/avx" && ./app 2>&1 | tr -d '\r')
    case "$got" in
        store=1|store=-1) ;;
        *)
            echo "  [FAIL] win_avx_unaligned_vector_move: an aligned ymm store to a 16-byte-aligned address faulted"
            echo "         printed '$got', expected 'store=1'"
            exit 1
            ;;
    esac
else
    if has_flag "$TMP/avx"; then
        echo "  [FAIL] win_avx_unaligned_vector_move: $FLAG reached a build outside Windows"
        exit 1
    fi
fi

for cf in "-O2" "-O2 -mavx2 -mno-avx"; do
    d="$TMP/noavx_$(printf '%s' "$cf" | tr -c 'a-z0-9' '_')"
    project "$d" "$cf"
    build "$d"
    if has_flag "$d"; then
        echo "  [FAIL] win_avx_unaligned_vector_move: $FLAG reached a build without AVX (cflags '$cf')"
        exit 1
    fi
done

echo "  [PASS] win_avx_unaligned_vector_move: AVX builds on Windows assemble aligned vector moves unaligned; other builds are unchanged"
