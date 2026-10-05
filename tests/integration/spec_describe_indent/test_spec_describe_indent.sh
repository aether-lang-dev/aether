#!/bin/sh
# #2380: std.spec indents each describe and test line by how deeply it is
# nested. It used to keep a running counter that describe() incremented and
# nothing decremented, so every sibling describe printed one level deeper than
# the one before, and a test written after a nested describe printed at the
# nested level.
#
# This is an integration test rather than a spec because the thing under test
# is the PRINTED TREE. probe.ae covers sibling describes, a nested describe, a
# test after the nested block, and skipped tests at both levels (skips have
# their own printing path). The tree must match exactly once colours are
# stripped.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"

[ -x "$AE" ] || { echo "  [SKIP] spec_describe_indent: ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT

cd "$ROOT" || exit 1
"$AE" run "$SCRIPT_DIR/probe.ae" > "$TMP/raw.log" 2>&1
rc=$?
# Strip ANSI colour codes and Windows line endings; keep the tree up to the
# blank line before the summary.
sed -e 's/\x1b\[[0-9;]*m//g' -e 's/\r$//' "$TMP/raw.log" | sed '/^$/,$d' > "$TMP/tree.log"

cat > "$TMP/expected.log" <<'EOF'
first
    ✓ a
second
    ✓ b
  nested
      ✓ c
      ⊘ d  (skipped: not here)
    ✓ e
third
    ⊘ f  (skipped: not here)
    ✓ g
EOF

if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] spec_describe_indent: probe exited $rc"
    sed 's/^/        /' "$TMP/raw.log"
    exit 1
fi
if ! diff -u "$TMP/expected.log" "$TMP/tree.log" > "$TMP/diff.log"; then
    echo "  [FAIL] spec_describe_indent: the printed tree is not indented by nesting"
    sed 's/^/        /' "$TMP/diff.log"
    exit 1
fi
echo "  [PASS] spec_describe_indent: siblings align, nesting indents, and a test after a nested describe returns to its suite's level"
