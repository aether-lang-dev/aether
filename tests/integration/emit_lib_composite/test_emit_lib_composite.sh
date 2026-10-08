#!/bin/sh
# Composite-return test for --emit=lib: the Aether script builds a nested
# config map (strings, ints, sub-map, list of strings) and the C host
# walks it via aether_config_* accessors.
#
# Windows too: consume.c loads the DLL with LoadLibrary. It used to skip
# there, so the accessors reading a string the script had copied went
# unseen until the POSIX jobs ran.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

LINK_FLAGS=""
case "$(uname -s 2>/dev/null)" in
    Darwin) LIB_EXT=".dylib" ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT) LIB_EXT=".dll" ;;
    *)      LIB_EXT=".so" ;;
esac

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

pass=0
fail=0

cd "$SCRIPT_DIR"
if ! AETHER_HOME="" "$ROOT/build/ae" build --emit=lib config.ae -o "$TMPDIR/libconfig" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] ae build --emit=lib failed"
    cat "$TMPDIR/build.log"
    fail=$((fail + 1))
    echo ""
    echo "emit_lib_composite: $pass passed, $fail failed"
    exit 1
fi

LIB_PATH=""
for candidate in "$TMPDIR/libconfig" "$TMPDIR/libconfig${LIB_EXT}"; do
    [ -f "$candidate" ] && { LIB_PATH="$candidate"; break; }
done
if [ -z "$LIB_PATH" ]; then
    echo "  [FAIL] no library produced"
    ls -la "$TMPDIR"
    fail=$((fail + 1))
    echo ""
    echo "emit_lib_composite: $pass passed, $fail failed"
    exit 1
fi

# Compile the consumer, linking against the aether_config.c accessors
# compiled into the library and the runtime/aether_config.h header. On
# Windows the DLL is found beside the consumer; elsewhere through the rpath.
case "$LIB_EXT" in
    .dll) ;;
    *)    LINK_FLAGS="-Wl,-rpath,$TMPDIR -ldl" ;;
esac
# shellcheck disable=SC2086
if ! gcc \
    -I"$ROOT/runtime" \
    "$SCRIPT_DIR/consume.c" \
    "$LIB_PATH" \
    $LINK_FLAGS \
    -o "$TMPDIR/consume" \
    2>"$TMPDIR/gcc.log"; then
    echo "  [FAIL] gcc could not compile consume.c"
    cat "$TMPDIR/gcc.log"
    fail=$((fail + 1))
    echo ""
    echo "emit_lib_composite: $pass passed, $fail failed"
    exit 1
fi

if "$TMPDIR/consume" "$LIB_PATH" >"$TMPDIR/run.out" 2>&1; then
    echo "  [PASS] composite config tree round-trip"
    pass=$((pass + 1))
else
    echo "  [FAIL] consume reported an error"
    cat "$TMPDIR/run.out"
    fail=$((fail + 1))
fi

echo ""
echo "emit_lib_composite: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
