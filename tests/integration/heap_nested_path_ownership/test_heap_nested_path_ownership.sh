#!/bin/sh
# A struct string field is owned the same way however the path is spelled (#1879).
#
# Follow-up to #1866. That fixed the setter-through-a-parameter case; a NESTED
# path (`o.inner.name = ...`) still emitted a bare store with no
# `_heap_<field>` tracker, so the inner struct's destructor believed it owned
# nothing and the string leaked -- while the identical write on the inner
# pointer released correctly. Ownership followed how the assignment was
# SPELLED rather than the type, which the reporter reached through the
# ordinary shape of a model holding a material holding a texture path.
#
# Running to completion is half the assertion (a double free aborts). The
# emitted ownership claim is checked directly too, so this still means
# something on the platforms with no leak checker in CI.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

if ! "$AETHERC" "$SCRIPT_DIR/prog.ae" "$TMP/out.c" > "$TMP/gen.log" 2>&1; then
    echo "  [FAIL] heap_nested_path_ownership: codegen failed"
    sed 's/^/        /' "$TMP/gen.log" | head -8
    exit 1
fi

# The direct write already claimed ownership before the fix; the nested one
# did not. Both must now, and `name` is written both ways in prog.ae, so
# count the claims rather than merely finding one.
claims=$(grep -c "_heap_name = 1" "$TMP/out.c" || true)
if [ "$claims" -lt 4 ]; then
    echo "  [FAIL] heap_nested_path_ownership: expected >=4 '_heap_name = 1' claims, found $claims"
    echo "         (set_through_outer, set_direct, the inline write, and the alias)"
    grep -n "_heap_name" "$TMP/out.c" | sed 's/^/        /' | head -8
    exit 1
fi
# Three levels deep: the object of the assignment is itself a member access.
if ! grep -q "_heap_tag = 1" "$TMP/out.c"; then
    echo "  [FAIL] heap_nested_path_ownership: a three-level path did not claim ownership"
    exit 1
fi

if ! "$AE" build "$SCRIPT_DIR/prog.ae" -o "$TMP/prog" > "$TMP/build.log" 2>&1; then
    echo "  [FAIL] heap_nested_path_ownership: build failed"
    sed 's/^/        /' "$TMP/build.log" | head -8
    exit 1
fi

if ! "$TMP/prog" > "$TMP/out.txt" 2>&1; then
    echo "  [FAIL] heap_nested_path_ownership: program aborted (double free?)"
    sed 's/^/        /' "$TMP/out.txt" | head -6
    exit 1
fi
grep -q "^all freed$" "$TMP/out.txt" || {
    echo "  [FAIL] heap_nested_path_ownership: program did not run to completion"
    sed 's/^/        /' "$TMP/out.txt" | head -6
    exit 1
}

# Where a leak checker exists, assert the actual property rather than only the
# generated text -- the tracker could be set and still not be read at free.
if command -v valgrind >/dev/null 2>&1; then
    if ! valgrind --error-exitcode=9 --leak-check=full --errors-for-leak-kinds=definite \
            "$TMP/prog" > "$TMP/vg.txt" 2>&1; then
        echo "  [FAIL] heap_nested_path_ownership: valgrind reported a definite leak"
        grep -E "definitely lost|Invalid" "$TMP/vg.txt" | sed 's/^/        /' | head -6
        exit 1
    fi
fi

echo "  [PASS] heap_nested_path_ownership: a field assigned through a nested path is owned like a direct one"
