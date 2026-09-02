#!/bin/sh
# An import that resolves to no module must be an error (#1858).
#
# It used to be accepted silently, so a typo stayed invisible until a call
# through the namespace failed somewhere else entirely, pointing at the call
# rather than the import.
#
# The second half matters as much as the first: std.heap is provided by the
# compiler and has no module directory, so it must still compile.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"

TMPDIR="$(mktemp -d)"
cleanup() { rm -rf "$TMPDIR"; }
trap cleanup EXIT
cd "$TMPDIR"

cat > missing.ae <<'AEEOF'
import a3d.does_not_exist

main() { println("compiled anyway") }
AEEOF

if "$AETHERC" missing.ae missing.c > missing.log 2>&1; then
    echo "  [FAIL] unresolved_import: an unresolvable import still compiles"
    exit 1
fi
if ! grep -q "unresolved import" missing.log; then
    echo "  [FAIL] unresolved_import: failed, but not with an unresolved-import diagnostic"
    cat missing.log
    exit 1
fi

cat > builtin.ae <<'AEEOF'
import std.heap

struct Box { n: int }

main() {
    b = heap.new(Box)
    b.n = 7
    println("${b.n}")
    heap.free(b)
}
AEEOF

if ! "$AETHERC" builtin.ae builtin.c > builtin.log 2>&1; then
    echo "  [FAIL] unresolved_import: std.heap (compiler-provided, no module dir) was rejected"
    cat builtin.log
    exit 1
fi

echo "  [PASS] unresolved_import: a bad import errors, a compiler-provided one still builds"
