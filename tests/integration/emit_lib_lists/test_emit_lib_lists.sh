#!/bin/sh
# List-return test for --emit=lib.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

# The host loads the library by path (dlopen, LoadLibrary on Windows) and
# links it; Windows finds the DLL beside the host, with no rpath or -ldl.
case "$(uname -s 2>/dev/null)" in
    Darwin) LIB_EXT=".dylib" ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT) LIB_EXT=".dll" ;;
    *)      LIB_EXT=".so" ;;
esac

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT
LINK_FLAGS=""
[ "$LIB_EXT" = .dll ] || LINK_FLAGS="-Wl,-rpath,$TMPDIR -ldl"
pass=0; fail=0

cd "$SCRIPT_DIR"
if ! AETHER_HOME="" "$ROOT/build/ae" build --emit=lib config.ae -o "$TMPDIR/liblists" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] ae build --emit=lib"; cat "$TMPDIR/build.log"; fail=$((fail + 1))
else
    LIB_PATH=""
    for c in "$TMPDIR/liblists" "$TMPDIR/liblists${LIB_EXT}"; do
        [ -f "$c" ] && { LIB_PATH="$c"; break; }
    done
    if [ -z "$LIB_PATH" ]; then
        echo "  [FAIL] no lib produced"; ls -la "$TMPDIR"; fail=$((fail + 1))
    elif ! gcc -I"$ROOT/runtime" "$SCRIPT_DIR/consume.c" "$LIB_PATH" $LINK_FLAGS -o "$TMPDIR/consume" 2>"$TMPDIR/gcc.log"; then
        echo "  [FAIL] gcc consume.c"; cat "$TMPDIR/gcc.log"; fail=$((fail + 1))
    elif "$TMPDIR/consume" "$LIB_PATH" >"$TMPDIR/run.out" 2>&1; then
        echo "  [PASS] list-return accessors"; pass=$((pass + 1))
    else
        echo "  [FAIL] consume errored"; cat "$TMPDIR/run.out"; fail=$((fail + 1))
    fi
fi

echo ""
echo "emit_lib_lists: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
