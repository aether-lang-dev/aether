#!/bin/sh
# Issue #338 — @derive(eq) synthesizer end-to-end.
#
# Verifies the synthesized T_eq function works correctly across
# every supported field type (int, string, bool) and on the
# empty-struct edge case.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

if ! AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/derive.ae" >"$TMPDIR/out.log" 2>&1; then
    echo "  [FAIL] ae run derive.ae exited non-zero"
    head -30 "$TMPDIR/out.log"
    exit 1
fi

for line in "Point ok" "User ok" "Empty ok" "DERIVE_EQ_DONE"; do
    if ! grep -qF "$line" "$TMPDIR/out.log"; then
        echo "  [FAIL] expected line missing: '$line'"
        echo "--- output:"; cat "$TMPDIR/out.log"
        exit 1
    fi
done

# Negative test: @derive(format) must surface the v1 limitation.
cat >"$TMPDIR/neg.ae" <<'EOF'
@derive(format)
struct X { x: int }
main() {}
EOF
if AETHER_HOME="$ROOT" "$AE" run "$TMPDIR/neg.ae" >"$TMPDIR/neg.log" 2>&1; then
    echo "  [FAIL] @derive(format) should error in v1, but compiled clean"
    cat "$TMPDIR/neg.log"
    exit 1
fi
if ! grep -q "not yet supported" "$TMPDIR/neg.log"; then
    echo "  [FAIL] expected 'not yet supported' diagnostic"
    head -10 "$TMPDIR/neg.log"
    exit 1
fi

# Negative test: unsupported field type → clear diagnostic.
# `ptr` is the simplest unsupported field type — opaque to the
# synthesizer because there's no canonical equality op.
cat >"$TMPDIR/neg2.ae" <<'EOF'
@derive(eq)
struct Bad {
    handle: ptr,
    n: int
}
main() {}
EOF
if AETHER_HOME="$ROOT" "$AE" run "$TMPDIR/neg2.ae" >"$TMPDIR/neg2.log" 2>&1; then
    echo "  [FAIL] @derive(eq) on ptr-field struct should error"
    cat "$TMPDIR/neg2.log"
    exit 1
fi
if ! grep -q "unsupported type" "$TMPDIR/neg2.log"; then
    echo "  [FAIL] expected 'unsupported type' diagnostic"
    head -10 "$TMPDIR/neg2.log"
    exit 1
fi

echo "  [PASS] derive_eq: positive 3-struct + 2 negative cases"
