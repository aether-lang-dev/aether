#!/bin/sh
# Regression: a top-level function taking a TYPED struct pointer (`*Thing`)
# must produce a compilable, correct --emit=lib wrapper. The wrapper's ABI
# presents the pointer as opaque, but the real function takes `Thing*`, so the
# wrapper must cast at the call — otherwise GCC 14+ rejects it under
# -Wincompatible-pointer-types and the library build fails outright.
# See asks/typed-struct-pointer-param-erased-in-export-wrapper.md.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] test_emit_lib_typed_ptr on Windows (uses POSIX dlopen)"; exit 0 ;;
esac
case "$(uname -s 2>/dev/null)" in
    Darwin) LIB_EXT=".dylib" ;;
    *)      LIB_EXT=".so" ;;
esac

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT
cd "$SCRIPT_DIR"

# 1. Build the library. Force -Werror on the incompatible-pointer class (via the
#    C backend `ae build` invokes) so a regression — the wrapper passing
#    AetherValue* to a Thing* function — fails the build here, not just warns.
#    This reproduces the exact GCC 14+ -Werror behaviour the ask describes,
#    independent of the default warning level. AE_CC is used as a command
#    prefix, so appending the flag threads it into the generated-C compile.
BASE_CC="${AE_CC:-${CC:-cc}}"
if ! AETHER_HOME="" AE_CC="$BASE_CC -Werror=incompatible-pointer-types" \
        "$ROOT/build/ae" build --emit=lib lib.ae -o "$TMPDIR/libthing" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] --emit=lib build failed (typed struct pointer wrapper?)"
    sed 's/^/        /' "$TMPDIR/build.log" | head -12
    exit 1
fi

LIB_PATH=""
for c in "$TMPDIR/libthing" "$TMPDIR/libthing${LIB_EXT}"; do
    [ -f "$c" ] && { LIB_PATH="$c"; break; }
done
[ -z "$LIB_PATH" ] && { echo "  [FAIL] no library produced"; exit 1; }

# 2. Compile + run the C host that dlopens it and calls aether_bump.
CC="${CC:-cc}"
if ! "$CC" -o "$TMPDIR/consume" consume.c -ldl >"$TMPDIR/cc.log" 2>&1; then
    echo "  [FAIL] could not compile consume.c"; sed 's/^/        /' "$TMPDIR/cc.log"; exit 1
fi

OUT="$("$TMPDIR/consume" "$LIB_PATH" 2>&1)"
if [ "$OUT" = "OK" ]; then
    echo "  [PASS] typed struct-pointer --emit=lib wrapper compiles and round-trips"
    exit 0
else
    echo "  [FAIL] consumer did not report OK:"
    echo "$OUT" | sed 's/^/        /'
    exit 1
fi
