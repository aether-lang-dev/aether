#!/bin/sh
# Regression: `--emit=lib` must carry exported module-level `const`
# declarations across the .so boundary (emit-lib-export-constants-ask.md).
#
# Builds const_lib.ae to a .so, copies ONLY the .so into an isolated dir (no
# source — otherwise source shadows the artifact and the test proves nothing),
# then builds uses_const_lib.ae against that dir and runs it. Pre-fix the
# consumer fails to type-check with `Undefined variable 'const_lib'`; post-fix
# it prints the function result and all five exported constants.
#
# Also asserts `ae lib-info` reports the constants (schema 1.2).

# The library is consumed through the binary-import path, which reads its
# catalog and links it: lib<module>.so / .dylib, or a .dll on Windows (#2297).
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) SO_EXT=".dll"; EXE=".exe" ;;
    Darwin)                          SO_EXT=".dylib"; EXE="" ;;
    *)                               SO_EXT=".so"; EXE="" ;;
esac

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] emit_lib_const: $AE not built"
    exit 0
fi

ISO="$(mktemp -d)"
trap 'rm -rf "$ISO"' EXIT

# 1. Build the library as a .so. The binimport prepass keys on the
#    lib<module>.so naming rule, so name it libconst_lib.so for
#    `import const_lib` to resolve it.
if ! AETHER_HOME="$ROOT" "$AE" build --emit=lib "$SCRIPT_DIR/const_lib.ae" \
        -o "$ISO/libconst_lib$SO_EXT" >"$ISO/build_lib.log" 2>&1; then
    echo "  [FAIL] emit_lib_const: building const_lib .so failed"
    head -15 "$ISO/build_lib.log"
    exit 1
fi

# 2. lib-info must list the constants (schema 1.2). Self-describing artifact.
INFO="$(AETHER_HOME="$ROOT" "$AE" lib-info "$ISO/libconst_lib$SO_EXT" 2>&1)"
if ! echo "$INFO" | grep -q "Constants:     5"; then
    echo "  [FAIL] emit_lib_const: lib-info did not report 5 constants"
    echo "$INFO" | head -20
    exit 1
fi
if ! echo "$INFO" | grep -Eq "K_STR: string = \"compaction: index unavailable\""; then
    echo "  [FAIL] emit_lib_const: lib-info string-const rendering wrong"
    echo "$INFO" | head -20
    exit 1
fi

# 3. Consume against the .so ONLY — copy just the consumer + the artifact into
#    the isolated dir (no const_lib.ae source there).
cp "$SCRIPT_DIR/uses_const_lib.ae" "$ISO/uses_const_lib.ae"
ACTUAL="$ISO/actual.txt"
if ! (cd "$ISO" && AETHER_HOME="$ROOT" "$AE" build --lib "$ISO" uses_const_lib.ae \
        -o "$ISO/consume") >"$ISO/build_consume.log" 2>&1; then
    echo "  [FAIL] emit_lib_const: consumer build against .so failed (constants not carried?)"
    head -20 "$ISO/build_consume.log"
    exit 1
fi
if ! "$ISO/consume$EXE" >"$ACTUAL" 2>&1; then
    echo "  [FAIL] emit_lib_const: consumer exited non-zero"
    head -10 "$ACTUAL"
    exit 1
fi

# 4. Verify every line (a Windows program writes CRLF).
tr -d '\r' < "$ACTUAL" > "$ACTUAL.lf" && mv "$ACTUAL.lf" "$ACTUAL"
ok=1
check() { echo "$ACTUAL" >/dev/null; grep -Fxq "$1" "$ACTUAL" || { echo "    missing: $1"; ok=0; }; }
check "fn=7"
check "int=42"
check "long=9000000000"
check "str=compaction: index unavailable"
check "t=true"
check "f=false"

if [ "$ok" = "1" ]; then
    echo "  [PASS] emit_lib_const: exported consts cross the .so boundary"
else
    echo "  [FAIL] emit_lib_const: consumer output missing expected lines"
    echo "--- actual ---"; cat "$ACTUAL"
    exit 1
fi
