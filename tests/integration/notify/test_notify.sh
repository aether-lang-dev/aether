#!/bin/sh
# notify() claim-check primitive — Aether script emits, C host receives.
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

cd "$SCRIPT_DIR"
if ! AETHER_HOME="" "$ROOT/build/ae" build --emit=lib config.ae -o "$TMPDIR/libnotify" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] ae build --emit=lib"; cat "$TMPDIR/build.log"; fail=$((fail + 1))
else
    LIB_PATH=""
    for c in "$TMPDIR/libnotify" "$TMPDIR/libnotify${LIB_EXT}"; do
        [ -f "$c" ] && { LIB_PATH="$c"; break; }
    done
    if [ -z "$LIB_PATH" ]; then
        echo "  [FAIL] no lib produced"; fail=$((fail + 1))
    elif ! gcc -I"$ROOT/runtime" "$SCRIPT_DIR/consume.c" "$LIB_PATH" $LINK_FLAGS -o "$TMPDIR/consume" 2>"$TMPDIR/gcc.log"; then
        echo "  [FAIL] gcc consume.c"; cat "$TMPDIR/gcc.log"; fail=$((fail + 1))
    elif "$TMPDIR/consume" "$LIB_PATH" >"$TMPDIR/run.out" 2>&1; then
        echo "  [PASS] notify() claim-check round-trip"; pass=$((pass + 1))
    else
        echo "  [FAIL] consume reported error"; cat "$TMPDIR/run.out"; fail=$((fail + 1))
    fi
fi

echo ""
echo "notify: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
