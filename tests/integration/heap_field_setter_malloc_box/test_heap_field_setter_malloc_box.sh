#!/bin/sh
# #1873: assigning a string field through a pointer-to-struct parameter must
# not free a garbage pointer when the box was hand-malloc'd (non-zeroed).
#
# Regression guard for the 0.624.0 crash that pinned the aether-ui line to
# v0.613.0: #1866 correctly made setter assignment take ownership, but the
# emitted store read an ownership tracker that a malloc'd box never
# initialised, then freed on it.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

OUT=$("$AE" run "$SCRIPT_DIR/prog.ae" 2>&1) || {
    echo "  [FAIL] heap_field_setter_malloc_box: program did not exit cleanly"
    echo "$OUT" | sed 's/^/    /' | head -10
    exit 1
}

case "$OUT" in
    *"survived: hello"*) ;;
    *)
        echo "  [FAIL] heap_field_setter_malloc_box: unexpected output"
        echo "$OUT" | sed 's/^/    /' | head -10
        exit 1
        ;;
esac

# The crash was a SIGSEGV, so an empty/short output with a zero status would
# also be wrong; the string check above covers it.
echo "  [PASS] heap_field_setter_malloc_box: setter assignment on a malloc'd box does not free garbage"
