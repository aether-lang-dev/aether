#!/bin/sh
# std.mem byte-level pointer access regression test.
#
# std.mem.get_byte / set_byte give Aether code byte-level access
# to caller-allocated raw pointers. Companion to std.bytes (which
# manages its own buffers). Driver use case: porting C codebases
# (mquickjs) that fundamentally operate on raw pointers — every
# UTF-8 decoder and bytecode interpreter does `((uint8_t*)p)[i]`.
#
# Exercises 26 cases across:
#   * byte get/set: round-trip, NULL defence, low-8-bit masking
#   * size_t-indexed byte get/set companions
#   * pointer / int / long / float-bits accessors
#   * clz32 / clz64 / udiv64_32
#   * sized typed-array accessors (int8/uint8/int16/uint16/uint32/
#     float32/float64) with sign extension and round-trip checks
#   * multi-sized read of the same byte buffer (TypedArray pattern)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] std_mem_byte_access: ae not built"
    exit 0
fi

cd "$ROOT" || exit 1

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

if ! AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/probe.ae" >"$TMPDIR/out.log" 2>&1; then
    echo "  [FAIL] std_mem_byte_access"
    tail -40 "$TMPDIR/out.log" | sed 's/^/    /'
    exit 1
fi

if ! grep -q "std_mem_byte_access: 26 passing, 0 failing" "$TMPDIR/out.log"; then
    echo "  [FAIL] std_mem_byte_access - not all 26 cases passed"
    tail -40 "$TMPDIR/out.log" | sed 's/^/    /'
    exit 1
fi

echo "  [PASS] std_mem_byte_access"
exit 0
