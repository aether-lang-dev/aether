#!/bin/sh
# A struct whose field is a struct from a transitively-reached module (#1856).
#
# Codegen emitted struct bodies in module-merge order, which puts an importing
# module's struct ahead of the one it imported whenever the inner one was only
# reached transitively. A forward typedef is enough for a pointer and not for a
# field, so the generated C failed to compile with "field has incomplete type".
#
# top -> mid -> leaf, where mid's struct holds leaf's struct by value.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
cleanup() { rm -rf "$TMPDIR"; }
trap cleanup EXIT

mkdir -p "$TMPDIR/src/leaf" "$TMPDIR/src/mid"
cat > "$TMPDIR/src/leaf/module.ae" <<'EOF'
exports (Point, point_new)
struct Point { x: float, y: float }
point_new(x: float, y: float) -> Point { return Point { x: x, y: y } }
EOF
cat > "$TMPDIR/src/mid/module.ae" <<'EOF'
import leaf
exports (Box, box_new)
struct Box { p: leaf.Point, n: int }
box_new(n: int) -> Box { return Box { p: leaf.point_new(1.0, 2.0), n: n } }
EOF
cat > "$TMPDIR/top.ae" <<'EOF'
import mid

main() {
    b = mid.box_new(7)
    println("${b.n}")
}
EOF

cd "$TMPDIR"
if ! AETHER_HOME="$ROOT" "$AE" build top.ae -o "$TMPDIR/top" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] transitive struct field did not compile:"
    grep -m3 -iE "error|incomplete" "$TMPDIR/build.log" | sed 's/^/         /'
    exit 1
fi

OUT=$("$TMPDIR/top" 2>&1 || true)
[ "$OUT" = "7" ] || { echo "  [FAIL] expected 7, got: $OUT"; exit 1; }

echo "  [PASS] module_struct_field_order: a struct field from a transitively imported module compiles and runs"
