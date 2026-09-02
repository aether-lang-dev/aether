#!/bin/sh
# Issue #521: a `@scoped` binding must not outlive its block. Each reject_*.ae
# below escapes a @scoped value a different way and MUST fail to build with the
# "@scoped ... escapes its block" diagnostic. (The allowed shapes — a derived
# scalar escaping, local-only use — are covered by the regression test
# test_issue521_scoped.ae.) The directory is pruned from the generic .ae
# runner so this driver owns the negative cells.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] scoped_escape: $AE not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

check_rejected() {
    name="$1"
    if "$AE" build "$SCRIPT_DIR/$name.ae" -o "$TMPDIR/out" >"$TMPDIR/$name.log" 2>&1; then
        echo "  [FAIL] scoped_escape: $name.ae compiled (should be rejected)"
        exit 1
    fi
    if ! grep -q "escapes its block" "$TMPDIR/$name.log"; then
        echo "  [FAIL] scoped_escape: $name.ae failed without the @scoped diagnostic"
        sed 's/^/    /' "$TMPDIR/$name.log" | head -10
        exit 1
    fi
}

check_rejected reject_return
check_rejected reject_alias
check_rejected reject_closure
check_rejected reject_container

echo "  [PASS] scoped_escape: return / alias / closure-capture / container-insert all rejected"
exit 0
