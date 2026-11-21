#!/bin/sh
# `ae build --target=<triple> --emit=staticlib` archives the C files the
# program's modules ship (`@source`, #2125) and the --extra files, as
# --emit=lib links them into a shared library.
#
# The archive held the program's object and the runtime's, and nothing
# else: a library importing contrib.quickjs left every qjs_* symbol
# undefined, so a consumer linking the .a with no Aether toolchain failed
# (servirtium-vcr asks/aether-staticlib-omits-source-files.md; its
# swift-bundle.sh compiled QuickJS by hand to make up for it).
#
# Asserts, for a host-matched target so the result can run here:
#   - the archive defines the @source file's function and the --extra
#     file's, where the host's nm can read it
#   - a C program links against the archive alone and runs, calling an
#     export that reaches both
#   - two modules whose C files share a basename both get in
#
# --emit=staticlib needs --target, so the host's own triple is named. Skips
# without zig. Cost: one runtime build for the target.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/portable.sh"
AE="$ROOT/build/ae"

if ! command -v zig >/dev/null 2>&1; then
    echo "  [SKIP] cross_staticlib_sources: zig not on PATH"
    exit 0
fi

SL_EXE=""
SL_LIBS="-lm"
case "$(uname -s)/$(uname -m)" in
    Darwin/arm64)         SL_TARGET=aarch64-macos ;;
    Darwin/x86_64)        SL_TARGET=x86_64-macos ;;
    Linux/aarch64)        SL_TARGET=aarch64-linux-musl ;;
    MINGW*|MSYS*|CYGWIN*)
        SL_TARGET=x86_64-windows
        SL_EXE=".exe"
        SL_LIBS="-lws2_32 -lcrypt32 -lgdi32 -luser32 -ladvapi32 -lbcrypt -ldbghelp" ;;
    Linux/x86_64)         SL_TARGET=x86_64-linux-musl ;;
    *)
        echo "  [SKIP] cross_staticlib_sources: $(uname -s)/$(uname -m) has no target triple whose binary runs here"
        exit 0 ;;
esac

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT INT TERM
export AETHER_CACHE_DIR="$T/cache"

# Two modules, each shipping a C file called util.c.
mkdir -p "$T/lib/alpha" "$T/lib/beta"
cat > "$T/lib/alpha/module.ae" <<'AE'
@source("util.c")
exports(alpha_value)
extern alpha_c_value() -> int
alpha_value() -> int { return alpha_c_value() }
AE
cat > "$T/lib/alpha/util.c" <<'C'
int alpha_c_value(void) { return 40; }
C
cat > "$T/lib/beta/module.ae" <<'AE'
@source("util.c")
exports(beta_value)
extern beta_c_value() -> int
beta_value() -> int { return beta_c_value() }
AE
cat > "$T/lib/beta/util.c" <<'C'
int beta_c_value(void) { return 2; }
C
cat > "$T/extra.c" <<'C'
int extra_c_value(void) { return 100; }
C
cat > "$T/embed.ae" <<'AE'
import alpha
import beta
extern extra_c_value() -> int

total() -> int {
    return alpha.alpha_value() + beta.beta_value() + extra_c_value()
}
AE

if ! (cd "$T" && "$AE" build --lib "$T/lib" --target="$SL_TARGET" --emit=staticlib --with=extern \
        --extra extra.c embed.ae -o "$T/libembed.a") > "$T/build.log" 2>&1; then
    echo "  [FAIL] cross_staticlib_sources: --emit=staticlib did not build"
    sed 's/^/    /' "$T/build.log" | head -20
    exit 1
fi

# The symbols, where the host nm reads the archive (zig ar writes a GNU
# archive, which BSD nm on macOS may not); the link below is the assertion
# that holds everywhere.
if command -v nm >/dev/null 2>&1 && nm "$T/libembed.a" > "$T/nm.out" 2>/dev/null; then
    for sym in alpha_c_value beta_c_value extra_c_value; do
        if ! grep -Eq "[0-9a-fA-F]+ T _?$sym\$" "$T/nm.out"; then
            echo "  [FAIL] cross_staticlib_sources: the archive does not define $sym"
            grep "$sym" "$T/nm.out" | sed 's/^/    /'
            exit 1
        fi
    done
fi

cat > "$T/host.c" <<'C'
#include <stdio.h>
extern int aether_total(void);
int main(void) { printf("total=%d\n", aether_total()); return 0; }
C
# shellcheck disable=SC2086  # SL_LIBS is a list of flags
if ! zig cc -target "$SL_TARGET" "$T/host.c" "$T/libembed.a" $SL_LIBS \
        -o "$T/host$SL_EXE" > "$T/link.log" 2>&1; then
    echo "  [FAIL] cross_staticlib_sources: a C program could not link the archive alone"
    sed 's/^/    /' "$T/link.log" | head -15
    exit 1
fi
RUN_RC=0
"$T/host$SL_EXE" > "$T/run.out" 2>&1 || RUN_RC=$?
RAN=$(tr -d '\r' < "$T/run.out")
if [ "$RUN_RC" -ne 0 ] || [ "$RAN" != "total=142" ]; then
    echo "  [FAIL] cross_staticlib_sources: the linked program printed '$RAN' (exit $RUN_RC), expected 'total=142'"
    exit 1
fi

echo "  [PASS] cross_staticlib_sources: @source and --extra C files are in the $SL_TARGET archive, which links and runs"
