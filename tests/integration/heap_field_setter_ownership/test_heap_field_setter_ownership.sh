#!/bin/sh
# A struct string field is owned the same way however it was assigned (#1866).
#
# The ownership wrapper only fired for an assignment written against a local
# heap-box pointer. A setter assigns through a pointer PARAMETER, so its store
# never set the `_heap_<field>` tracker and the box's destructor believed it
# owned nothing. There was therefore no correct way to write a destructor:
# freeing the field by hand double-frees in the local case and leaks in the
# setter case.
#
# Running to completion is half the assertion (a double free aborts). The
# emitted ownership claim is checked directly too, so this still means
# something on platforms with no leak checker.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

if ! "$AETHERC" "$SCRIPT_DIR/prog.ae" "$TMP/out.c" > "$TMP/gen.log" 2>&1; then
    echo "  [FAIL] heap_field_setter_ownership: codegen failed"
    sed 's/^/        /' "$TMP/gen.log" | head -8
    exit 1
fi

# Style A (local pointer) and style B (setter through a parameter) must BOTH
# claim ownership. Before the fix only the first appeared.
if ! grep -q "_heap_path = 1" "$TMP/out.c"; then
    echo "  [FAIL] heap_field_setter_ownership: local-pointer assignment did not claim ownership"
    exit 1
fi
if ! grep -q "_heap_name = 1" "$TMP/out.c"; then
    echo "  [FAIL] heap_field_setter_ownership: setter assignment did not claim ownership"
    exit 1
fi

if ! "$AE" build "$SCRIPT_DIR/prog.ae" -o "$TMP/prog" > "$TMP/build.log" 2>&1; then
    echo "  [FAIL] heap_field_setter_ownership: build failed"
    sed 's/^/        /' "$TMP/build.log" | head -8
    exit 1
fi

if ! "$TMP/prog" > "$TMP/out.txt" 2>&1; then
    echo "  [FAIL] heap_field_setter_ownership: program aborted (double free?)"
    sed 's/^/        /' "$TMP/out.txt" | head -6
    exit 1
fi
grep -q "^both freed$" "$TMP/out.txt" || {
    echo "  [FAIL] heap_field_setter_ownership: program did not run to completion"
    sed 's/^/        /' "$TMP/out.txt" | head -6
    exit 1
}

echo "  [PASS] heap_field_setter_ownership: a field assigned through a setter is owned like one assigned on a local"
