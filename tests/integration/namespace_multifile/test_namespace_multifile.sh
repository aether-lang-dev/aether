#!/bin/sh
# Multi-script namespace — three siblings share one manifest.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s 2>/dev/null)" in
    Darwin) LIB_EXT=".dylib" ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT) LIB_EXT=".dll" ;;
    *)      LIB_EXT=".so" ;;
esac

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT
pass=0; fail=0

# consume.c loads the library with dlopen, or LoadLibrary on Windows, where
# the DLL is found beside the consumer and so needs no rpath and no -ldl.
LINK_FLAGS=""
[ "$LIB_EXT" = .dll ] || LINK_FLAGS="-Wl,-rpath,$TMPDIR -ldl"

cp "$SCRIPT_DIR"/manifest.ae "$SCRIPT_DIR"/add.ae "$SCRIPT_DIR"/multiply.ae "$SCRIPT_DIR"/subtract.ae "$TMPDIR/"

cd "$TMPDIR"
if ! AETHER_HOME="" "$ROOT/build/ae" build --namespace . >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] ae build --namespace ."; cat "$TMPDIR/build.log"; fail=$((fail + 1))
else
    LIB_PATH=""
    for c in "$TMPDIR/lib"*"${LIB_EXT}"; do
        [ -f "$c" ] && { LIB_PATH="$c"; break; }
    done
    if [ -z "$LIB_PATH" ]; then
        echo "  [FAIL] no lib"; fail=$((fail + 1))
    elif ! gcc -I"$ROOT/runtime" "$SCRIPT_DIR/consume.c" "$LIB_PATH" $LINK_FLAGS -o "$TMPDIR/consume" 2>"$TMPDIR/gcc.log"; then
        echo "  [FAIL] gcc"; cat "$TMPDIR/gcc.log"; fail=$((fail + 1))
    elif "$TMPDIR/consume" "$LIB_PATH" >"$TMPDIR/run.out" 2>&1; then
        echo "  [PASS] three sibling scripts share one namespace"; pass=$((pass + 1))
    else
        echo "  [FAIL] consume errored"; cat "$TMPDIR/run.out"; fail=$((fail + 1))
    fi
fi

echo ""
echo "namespace_multifile: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
