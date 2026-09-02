#!/bin/sh
# heap.new / heap.free across function boundaries (#1860).
#
# Two bugs met here. `return heap.new(T)` outside main lost the node's pointer
# type and fell back to calloc(1, sizeof(void)) -- a ONE BYTE allocation for
# the whole struct, so field writes ran past the end of it. And box provenance
# was per-function, so `b = build_box()` lost it and `b.name = ...` became a
# bare store that never set the _heap_<field> tracker, leaving the box's
# destructor believing it owned nothing.
#
# The generated C is asserted directly because the leak only reproduces under a
# leak checker, which is not available on every platform this suite runs on.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

TMPDIR="$(mktemp -d)"
cleanup() { rm -rf "$TMPDIR"; }
trap cleanup EXIT
cd "$TMPDIR"

cat > main.ae <<'AEEOF'
import std.heap
import std.string

struct Box { name: string }

build_box() -> *Box { return heap.new(Box) }
dispose(b: *Box) { heap.free(b) }

main() {
    b = build_box()
    b.name = string.copy("hello")
    println("${b.name}")
    dispose(b)
}
AEEOF

if ! "$AETHERC" main.ae out.c > gen.log 2>&1; then
    echo "  [FAIL] heap_box_across_functions: codegen failed"
    cat gen.log
    exit 1
fi

if grep -q "sizeof(void)" out.c; then
    echo "  [FAIL] heap_box_across_functions: heap.new allocated sizeof(void), not the struct"
    exit 1
fi
if ! grep -q "sizeof(Box)" out.c; then
    echo "  [FAIL] heap_box_across_functions: no sizeof(Box) allocation emitted"
    exit 1
fi
if ! grep -q "_heap_name = 1" out.c; then
    echo "  [FAIL] heap_box_across_functions: field assignment did not claim ownership"
    exit 1
fi

if ! "$AE" build main.ae -o prog > build.log 2>&1; then
    echo "  [FAIL] heap_box_across_functions: build failed"
    cat build.log
    exit 1
fi
out="$(./prog)"
if [ "$out" != "hello" ]; then
    echo "  [FAIL] heap_box_across_functions: got '$out', want 'hello'"
    exit 1
fi

echo "  [PASS] heap_box_across_functions: the box is sized right and owns its string field"
