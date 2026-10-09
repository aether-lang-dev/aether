#!/bin/sh
# Test: SWIG-generated Python bindings call aether_* functions in a
# --emit=lib .so.
#
# Requires: swig, python3 with the Python dev headers (for compiling the
# SWIG wrapper). Skips with [SKIP] if any dependency is missing, so the
# test is non-blocking for environments without a SWIG toolchain.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

# Dependency probes — skip cleanly if any is missing.
if ! command -v swig >/dev/null 2>&1; then
    echo "  [SKIP] test_emit_lib_swig (swig not installed)"
    exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "  [SKIP] test_emit_lib_swig (python3 not installed)"
    exit 0
fi
# python3-config gives us the include path + link flags for the dev headers.
if ! command -v python3-config >/dev/null 2>&1; then
    echo "  [SKIP] test_emit_lib_swig (python3-config not installed — needs python3-dev)"
    exit 0
fi

case "$(uname -s 2>/dev/null)" in
    Darwin) LIB_EXT=".dylib" ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT) LIB_EXT=".dll" ;;
    *)      LIB_EXT=".so" ;;
esac

# The extension module is named the way this Python imports one: .pyd on
# Windows, and on macOS .so, not the .dylib a plain shared library gets.
PY_EXT="$(python3 -c 'import sysconfig; print(sysconfig.get_config_var("EXT_SUFFIX") or "")' 2>/dev/null)"
if [ -z "$PY_EXT" ]; then
    echo "  [SKIP] test_emit_lib_swig (python3 reports no extension-module suffix)"
    exit 0
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

pass=0
fail=0

# 1. Build the Aether lib.
cd "$SCRIPT_DIR"
if ! AETHER_HOME="" "$ROOT/build/ae" build --emit=lib config.ae -o "$TMPDIR/libaether_sample" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] ae build --emit=lib failed"
    cat "$TMPDIR/build.log"
    fail=$((fail + 1))
    echo ""
    echo "emit_lib_swig: $pass passed, $fail failed"
    [ "$fail" -eq 0 ]
    exit $?
fi

LIB_PATH=""
for candidate in "$TMPDIR/libaether_sample" "$TMPDIR/libaether_sample${LIB_EXT}"; do
    [ -f "$candidate" ] && { LIB_PATH="$candidate"; break; }
done
if [ -z "$LIB_PATH" ]; then
    echo "  [FAIL] ae build did not produce a lib file"
    ls -la "$TMPDIR"
    fail=$((fail + 1))
    echo ""
    echo "emit_lib_swig: $pass passed, $fail failed"
    exit 1
fi

# 2. Run swig to generate the Python wrapper .c file.
cp "$SCRIPT_DIR/aether_lib.i" "$TMPDIR/"
cd "$TMPDIR"
if ! swig -python -o aether_lib_wrap.c aether_lib.i >"$TMPDIR/swig.log" 2>&1; then
    echo "  [FAIL] swig -python failed"
    cat "$TMPDIR/swig.log"
    fail=$((fail + 1))
    echo ""
    echo "emit_lib_swig: $pass passed, $fail failed"
    exit 1
fi

# 3. Compile the wrapper into a Python extension module linked against our lib.
PY_INCLUDE="$(python3-config --includes)"
PY_LDFLAGS="$(python3-config --ldflags)"
if ! gcc -fPIC -shared $PY_INCLUDE aether_lib_wrap.c "$LIB_PATH" $PY_LDFLAGS -o "_aether_lib${PY_EXT}" 2>"$TMPDIR/gcc.log"; then
    echo "  [FAIL] gcc could not build the Python extension"
    cat "$TMPDIR/gcc.log"
    fail=$((fail + 1))
    echo ""
    echo "emit_lib_swig: $pass passed, $fail failed"
    exit 1
fi

# 4. Run the Python script that imports the module and asserts round-trip.
cp "$SCRIPT_DIR/roundtrip.py" "$TMPDIR/"
# Set LD_LIBRARY_PATH so the Python extension's dependency on libaether_sample
# resolves. Windows needs nothing: Python loads an extension with its own
# directory on the DLL search path, and the library sits beside it.
LIB_DIR="$(dirname "$LIB_PATH")"
if LD_LIBRARY_PATH="$LIB_DIR:$LD_LIBRARY_PATH" python3 roundtrip.py >"$TMPDIR/run.out" 2>&1; then
    echo "  [PASS] SWIG Python bindings round-trip Aether lib"
    pass=$((pass + 1))
else
    echo "  [FAIL] python roundtrip.py failed"
    cat "$TMPDIR/run.out"
    fail=$((fail + 1))
fi

echo ""
echo "emit_lib_swig: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
