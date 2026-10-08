#!/bin/sh
# Kind-discriminator + safe deep-free integration test.
#
# Aether script builds a tree (map + nested map + nested list +
# scalar int + scalar string). C host probes every value's kind
# via the aether_value_kind() / _is_map() / _is_list() predicates
# — including adversarial low-address scalars (`(AetherValue*)42`)
# that would crash a naive blind-deref implementation. Then calls
# aether_config_free_deep() and asserts the magic-clear-on-free
# defends against use-after-free probes.
#
# What this proves:
#   - low-address guard works (process does not segfault on
#     fake_int_value = 42)
#   - magic check correctly identifies map vs list
#   - schema-mismatch (calling _get_map on an int slot) returns
#     a value the kind probe correctly classifies as UNKNOWN
#     instead of crashing the host
#   - deep-free walks recursively and clears magic on the way out

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

# The host loads the library by path (dlopen, LoadLibrary on Windows) and
# links it for the aether_value_* accessors. Windows finds the DLL beside
# the host, so it needs no rpath and no -ldl.
case "$(uname -s 2>/dev/null)" in
    Darwin) LIB_EXT=".dylib" ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT) LIB_EXT=".dll" ;;
    *)      LIB_EXT=".so" ;;
esac

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT
LINK_FLAGS=""
[ "$LIB_EXT" = .dll ] || LINK_FLAGS="-Wl,-rpath,$TMPDIR -ldl"

pass=0
fail=0

cd "$SCRIPT_DIR"
if ! AETHER_HOME="" "$ROOT/build/ae" build --emit=lib config.ae -o "$TMPDIR/libtree" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] ae build --emit=lib failed"
    cat "$TMPDIR/build.log"
    fail=$((fail + 1))
    echo ""
    echo "emit_lib_kind_safe: $pass passed, $fail failed"
    exit 1
fi

LIB_PATH=""
for candidate in "$TMPDIR/libtree" "$TMPDIR/libtree${LIB_EXT}"; do
    [ -f "$candidate" ] && { LIB_PATH="$candidate"; break; }
done
if [ -z "$LIB_PATH" ]; then
    echo "  [FAIL] no library produced"
    ls -la "$TMPDIR"
    fail=$((fail + 1))
    echo ""
    echo "emit_lib_kind_safe: $pass passed, $fail failed"
    exit 1
fi

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
    echo "emit_lib_kind_safe: $pass passed, $fail failed"
    exit 1
fi

if "$TMPDIR/consume" "$LIB_PATH" >"$TMPDIR/run.out" 2>&1; then
    echo "  [PASS] kind probes safe on adversarial inputs; deep-free walks tree; magic-clear-on-free works"
    pass=$((pass + 1))
else
    echo "  [FAIL] consume reported an error or crashed"
    cat "$TMPDIR/run.out"
    fail=$((fail + 1))
fi

echo ""
echo "emit_lib_kind_safe: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
