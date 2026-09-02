#!/bin/sh
# An imported const bound to a local outside main (#1857).
#
# A numeric literal parses as UNKNOWN ("let inference decide int or float") and
# the const node is typed in the second pass, but imported consts land at the
# END of the merged tree, so a function binding one to a local was checked
# first and inferred UNKNOWN. Codegen then defaulted the local to int: right by
# luck for an int const, a SILENT TRUNCATION for a float one.
#
# This asserts the value, not the warning: 2.5 * 2.0 is 5, and the bug printed 4.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
cleanup() { rm -rf "$TMPDIR"; }
trap cleanup EXIT

mkdir -p "$TMPDIR/src/pk/fl"
cat > "$TMPDIR/src/pk/fl/module.ae" <<'AEEOF'
exports (SCALE)
const SCALE = 2.5
AEEOF

cat > "$TMPDIR/main.ae" <<'AEEOF'
import pk.fl

scaled() -> float {
    k = fl.SCALE
    return k * 2.0
}

main() { println("${scaled()}") }
AEEOF

cd "$TMPDIR"
if ! "$AE" build main.ae -o prog > build.log 2>&1; then
    echo "  [FAIL] imported_const_type: build failed"
    cat build.log
    exit 1
fi

out="$(./prog)"
if [ "$out" != "5" ]; then
    echo "  [FAIL] imported_const_type: got '$out', want '5' (float const truncated to int)"
    exit 1
fi

# The spurious diagnostic the issue was filed about must be gone too.
if "$ROOT/build/aetherc" main.ae out.c 2>&1 | grep -q "unresolved type in codegen"; then
    echo "  [FAIL] imported_const_type: still warns 'unresolved type in codegen'"
    exit 1
fi

echo "  [PASS] imported_const_type: an imported float const keeps its type outside main"
