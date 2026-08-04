#!/bin/sh
# Issue #525: a parameter `where` clause is a runtime precondition — a
# violation is a hard panic (not a recoverable error). probe_violation.ae
# calls divide(10, 0), violating `where b != 0`; it MUST build, then ABORT at
# runtime with the precondition message. The satisfied path is covered by the
# regression test test_issue525_where.ae. Pruned from the generic .ae runner
# (it intentionally panics) so this driver owns it.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
if [ ! -x "$AE" ]; then echo "  [SKIP] where_clause: $AE not built"; exit 0; fi
TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

if ! "$AE" build "$SCRIPT_DIR/probe_violation.ae" -o "$TMPDIR/probe" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] where_clause: probe_violation.ae failed to build"
    sed 's/^/    /' "$TMPDIR/build.log" | head -10; exit 1
fi
"$TMPDIR/probe" >"$TMPDIR/run.log" 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "  [FAIL] where_clause: violation did not panic (exit 0)"; exit 1
fi
if ! grep -q "precondition_violation: b != 0" "$TMPDIR/run.log"; then
    echo "  [FAIL] where_clause: panic lacked the precondition message"
    sed 's/^/    /' "$TMPDIR/run.log" | head -10; exit 1
fi
echo "  [PASS] where_clause: parameter \`where\` violation panics with the condition"
exit 0
