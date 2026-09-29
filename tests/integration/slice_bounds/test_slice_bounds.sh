#!/bin/sh
# Issue #1286: a slice carries its length, so an out-of-range index or
# sub-range is a runtime panic naming the source line, the index / range and
# the length — never a silent out-of-bounds read. A failed check is a
# regular panic (`try` catches it). Ill-typed slicing is rejected at compile
# time. The accepted shapes are covered by tests/syntax/test_slices.ae.
# Pruned from the generic .ae runner (these sources are expected to fail).
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
if [ ! -x "$AE" ]; then echo "  [SKIP] slice_bounds: $AE not built"; exit 0; fi
TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

# $1 = source basename, $2 = extended regex the diagnostic must match
check_rejected() {
    name="$1"; want="$2"
    if "$AE" build "$SCRIPT_DIR/$name.ae" -o "$TMPDIR/out" >"$TMPDIR/$name.log" 2>&1; then
        echo "  [FAIL] slice_bounds: $name.ae compiled (should be rejected)"; exit 1
    fi
    if ! grep -qiE "$want" "$TMPDIR/$name.log"; then
        echo "  [FAIL] slice_bounds: $name.ae rejected, but not for the stated reason"
        echo "         expected a diagnostic matching: $want"
        sed 's/^/    /' "$TMPDIR/$name.log" | head -8; exit 1
    fi
}

# $1 = source basename, $2 = extended regex the panic message must match
check_panics() {
    name="$1"; want="$2"
    if ! "$AE" build "$SCRIPT_DIR/$name.ae" -o "$TMPDIR/$name" >"$TMPDIR/$name.build.log" 2>&1; then
        echo "  [FAIL] slice_bounds: $name.ae did not build"
        sed 's/^/    /' "$TMPDIR/$name.build.log" | head -8; exit 1
    fi
    if "$TMPDIR/$name" >"$TMPDIR/$name.run.log" 2>&1; then
        echo "  [FAIL] slice_bounds: $name exited 0 (should panic)"; exit 1
    fi
    if ! grep -qE "$want" "$TMPDIR/$name.run.log"; then
        echo "  [FAIL] slice_bounds: $name panicked, but not with the expected message"
        echo "         expected: $want"
        sed 's/^/    /' "$TMPDIR/$name.run.log" | head -8; exit 1
    fi
}

check_rejected reject_bound_not_int 'slice bound must be an integer'
check_rejected reject_slice_of_int  'sub-slicing is defined for'

check_panics panic_index          'panic_index.ae:5: slice index 3 out of range for length 3'
check_panics panic_negative_index 'panic_negative_index.ae:5: slice index -1 out of range for length 3'
check_panics panic_range_end      'panic_range_end.ae:3: slice range 2\.\.5 out of range for length 3'
check_panics panic_range_inverted 'panic_range_inverted.ae:5: slice range 2\.\.1 out of range for length 3'

if ! "$AE" build "$SCRIPT_DIR/caught_by_try.ae" -o "$TMPDIR/caught" >"$TMPDIR/caught.build.log" 2>&1; then
    echo "  [FAIL] slice_bounds: caught_by_try.ae did not build"
    sed 's/^/    /' "$TMPDIR/caught.build.log" | head -8; exit 1
fi
if ! "$TMPDIR/caught" >"$TMPDIR/caught.run.log" 2>&1; then
    echo "  [FAIL] slice_bounds: caught_by_try exited non-zero"; exit 1
fi
if ! grep -q 'caught: .*slice index 9 out of range for length 3' "$TMPDIR/caught.run.log" || \
   grep -q 'NOT REACHED' "$TMPDIR/caught.run.log"; then
    echo "  [FAIL] slice_bounds: try/catch did not catch the bounds panic"
    sed 's/^/    /' "$TMPDIR/caught.run.log" | head -8; exit 1
fi

echo "  [PASS] slice_bounds: ill-typed slicing rejected; index, negative index, range end, inverted range panic; try catches"
exit 0
