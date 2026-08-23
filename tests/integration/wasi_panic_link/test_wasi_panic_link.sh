#!/bin/sh
# `--target=wasm32-wasi` links code that uses panic / try / catch.
#
# aether_panic.c guarded its crash handler with !defined(__wasi__), but
# aether_panic.h's setjmp MACRO SELECTION did not. WASI is hosted
# (__STDC_HOSTED__ == 1) and does not define __EMSCRIPTEN__, so it fell into
# the POSIX arm and got _setjmp/_longjmp -- which wasi-libc declares but never
# implements. That is a LINK error, not a compile error, so it surfaced only
# at the very end of a cross build:
#
#     wasm-ld: error: libaether.a(aether_panic.o): undefined symbol: _longjmp
#
# The fixture deliberately USES try/catch/panic: a wasi library without them
# links fine even with the selection wrong, because nothing references
# aether_panic.o. That is why this needs its own fixture rather than reusing
# cross_emit_lib's.
#
# Asserts:
#   - the module links at all (the regression)
#   - it is a real wasm binary
#   - the exported functions are present
#   - no _setjmp/_longjmp import survives into the module
#
# Skips without zig. Cost: ONE cross link (~90 TUs; there is no per-target
# archive cache), so this does exactly one and asserts everything on it.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] wasi_panic_link: ae not built"
    exit 0
fi
if ! command -v zig >/dev/null 2>&1; then
    echo "  [SKIP] wasi_panic_link: zig not on PATH"
    exit 0
fi

TMPDIR_T="$(mktemp -d)"
cleanup() { rm -rf "$TMPDIR_T"; }
trap cleanup EXIT

OUT="$TMPDIR_T/paniclib.wasm"
BUILD_LOG="$TMPDIR_T/build.log"

if ! "$AE" build --target=wasm32-wasi --emit=lib \
        "$SCRIPT_DIR/paniclib.ae" -o "$OUT" >"$BUILD_LOG" 2>&1; then
    echo "  [FAIL] wasi_panic_link: cross build failed"
    # The regression's signature, surfaced directly when it recurs.
    if grep -q '_longjmp\|_setjmp' "$BUILD_LOG"; then
        echo "         setjmp selection regressed: aether_panic.h's macro"
        echo "         arms must special-case __wasi__ (see the header)."
    fi
    sed -n '1,20p' "$BUILD_LOG"
    exit 1
fi

[ -f "$OUT" ] || { echo "  [FAIL] wasi_panic_link: no output at $OUT"; exit 1; }

# A real wasm module, not an empty file or a host artifact.
case "$(file -b "$OUT" 2>/dev/null)" in
    *WebAssembly*) ;;
    *)
        echo "  [FAIL] wasi_panic_link: not a wasm module:"
        echo "         $(file -b "$OUT" 2>/dev/null)"
        exit 1
        ;;
esac

# The functions must actually be exported -- a module that links but exports
# nothing would satisfy every check above and be useless.
for sym in aether_risky aether_safe; do
    if ! strings "$OUT" | grep -q "$sym"; then
        echo "  [FAIL] wasi_panic_link: $sym missing from the module"
        exit 1
    fi
done

# Nothing should still be reaching for the unimplemented pair.
if strings "$OUT" | grep -qE '^_setjmp$|^_longjmp$'; then
    echo "  [FAIL] wasi_panic_link: module still references _setjmp/_longjmp"
    exit 1
fi

echo "  [PASS] wasi_panic_link: wasm32-wasi links panic/try/catch, exports present"
