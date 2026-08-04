#!/bin/sh
# Regression (#1378 part B): unrecoverable failures lead with a stable,
# greppable category token, then the human detail. Before this, each panic site
# invented its own wording, so CI and downstream triage could not match on the
# failure class without pinning prose that was free to change.
#
# The tokens are the contract. This test pins them; the detail after the colon
# is deliberately not asserted, because that part IS free to change.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] panic_categories: ae not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT
fail=0

# $1 = fixture basename, $2 = category token that must lead the panic
expect_category() {
    name="$1"
    token="$2"
    if ! "$AE" build "$SCRIPT_DIR/$name.ae" -o "$TMPDIR/$name" >"$TMPDIR/$name.build" 2>&1; then
        echo "  [FAIL] panic_categories: $name.ae did not build"
        sed 's/^/        /' "$TMPDIR/$name.build" | head -8
        fail=1
        return
    fi
    "$TMPDIR/$name" >"$TMPDIR/$name.log" 2>&1
    if [ $? -eq 0 ]; then
        echo "  [FAIL] panic_categories: $name.ae exited 0, expected a panic"
        fail=1
        return
    fi
    if ! grep -q "$token:" "$TMPDIR/$name.log"; then
        echo "  [FAIL] panic_categories: $name.ae did not lead with '$token:'"
        sed 's/^/        /' "$TMPDIR/$name.log" | head -6
        fail=1
    fi
}

expect_category precondition precondition_violation
expect_category forced_none  forced_unwrap_none

if [ "$fail" -eq 0 ]; then
    echo "  [PASS] panic_categories: failures lead with their stable category token"
fi
exit $fail
