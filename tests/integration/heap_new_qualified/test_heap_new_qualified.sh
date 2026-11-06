#!/bin/sh
# heap.new with another module's struct: `heap.new(pkg.T)`.
#
# The type of heap.new's argument was read as one identifier, so a struct
# from an imported module could not be named the way it is everywhere else
# (`*pkg.T`, `pkg.T { ... }`, #946): `heap.new(pkg.Point)` stopped with
# "Expected RIGHT_PAREN, got DOT", and a consumer needed a selective
# `import pkg (Point)` (which fails when two modules both have a `Point`).
#
# Fixture: lib/ga.ae and lib/gb.ae each define a different `Shape`, and
# main.ae a third; heap.new boxes each one by its qualifier (gb's has a
# string field, released by heap.free). missing.ae, unimported.ae and
# unexported.ae each hold one qualifier error, reported as for `*pkg.T`.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
fail=0

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run main.ae 2>&1 )
got=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tr '\n' ' ' | sed 's/ *$//')
if [ "$got" != "12 12 hex 9" ]; then
    echo "  [FAIL] heap_new_qualified: expected '12 12 hex 9', got:"
    echo "$out" | head -20 | sed 's/^/          /'
    fail=1
fi

# expect_error FILE LINE MESSAGE: the build fails, at FILE:LINE, with MESSAGE
expect_error() {
    out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" build "$1" -o "${TMPDIR:-/tmp}/hnq_$$" 2>&1 )
    rc=$?
    rm -f "${TMPDIR:-/tmp}/hnq_$$"
    if [ "$rc" -eq 0 ] || ! printf '%s\n' "$out" | grep -qF -- "$3" ||
       ! printf '%s\n' "$out" | grep -qF -- "--> $1:$2:"; then
        echo "  [FAIL] heap_new_qualified: $1 should fail at line $2 with: $3"
        echo "$out" | head -12 | sed 's/^/          /'
        fail=1
    fi
}
expect_error missing.ae 3 "module 'ga' has no type 'Missing'"
expect_error unimported.ae 3 "unknown module 'render' in the type 'render.Shape'"
expect_error unexported.ae 4 "'Scratch' is not exported from module 'gb'"

[ "$fail" -eq 0 ] || exit 1
echo "  [PASS] heap_new_qualified: heap.new(pkg.T) boxes that module's struct; a bad qualifier is reported"
exit 0
