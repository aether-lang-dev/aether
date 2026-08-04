#!/bin/sh
# Integration test for `@c_struct ... @c_verify` (#1242).
#
# A @c_struct overlay's field offsets are written by hand, because Aether never
# reads the C header. Nothing checked them, so a field inserted upstream shifted
# the layout and the overlay kept reading the old offset: correct type, wrong
# bytes, no diagnostic. @c_verify emits a _Static_assert per field so the C
# compiler checks the declared offset AND width against the real layout.
#
# Asserts, in order:
#   1. a correct overlay builds and reads the header's values;
#   2. a wrong OFFSET fails the build (the silent-misread case);
#   3. a wrong WIDTH fails the build (declaring int for a long reads half of it,
#      the accessor-width footgun @c_struct exists to remove).
#
# 2 and 3 are what give 1 its meaning: without them this test would still pass
# if @c_verify emitted nothing at all.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] c_struct_verify: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"; rm -f "$SCRIPT_DIR/bad_offset.ae" "$SCRIPT_DIR/bad_width.ae"' EXIT

cd "$SCRIPT_DIR" || exit 1

# (1) the correct overlay builds and runs.
if ! "$AE" build probe.ae -o "$TMP/probe" >"$TMP/build.log" 2>&1; then
    echo "  [FAIL] c_struct_verify: correct overlay did not build"
    sed 's/^/        /' "$TMP/build.log" | head -15
    exit 1
fi
out=$("$TMP/probe" 2>&1)
if [ "$out" != "ok" ]; then
    echo "  [FAIL] c_struct_verify: probe printed '$out', want 'ok'"
    exit 1
fi

# (2) a wrong offset must fail the build.
sed 's/length: uint64 @8/length: uint64 @16/' probe.ae > bad_offset.ae
if "$AE" build bad_offset.ae -o "$TMP/bad_off" >"$TMP/off.log" 2>&1; then
    echo "  [FAIL] c_struct_verify: a wrong offset still built"
    exit 1
fi
if ! grep -q "declared at offset" "$TMP/off.log"; then
    echo "  [FAIL] c_struct_verify: wrong offset failed without the offset assertion"
    sed 's/^/        /' "$TMP/off.log" | head -15
    exit 1
fi

# (3) a wrong width must fail the build.
sed 's/slen: uint32 @16/slen: uint64 @16/' probe.ae > bad_width.ae
if "$AE" build bad_width.ae -o "$TMP/bad_w" >"$TMP/w.log" 2>&1; then
    echo "  [FAIL] c_struct_verify: a wrong field width still built"
    exit 1
fi
if ! grep -q "bytes wide" "$TMP/w.log"; then
    echo "  [FAIL] c_struct_verify: wrong width failed without the width assertion"
    sed 's/^/        /' "$TMP/w.log" | head -15
    exit 1
fi

echo "  [PASS] c_struct_verify: overlay offsets and widths checked against the C header"
