#!/bin/sh
# Issue #239 regression: std.http.client redirect support API contract.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

ACTUAL="$TMPDIR/actual.txt"
if ! AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/redirects_api.ae" >"$ACTUAL" 2>"$TMPDIR/err.log"; then
    echo "  [FAIL] ae run exited non-zero"
    cat "$TMPDIR/err.log" | head -10
    exit 1
fi

# The script self-reports PASS or FAIL on its last line.
LAST_LINE=$(tail -1 "$ACTUAL")
case "$LAST_LINE" in
    PASS:*)
        echo "  $LAST_LINE"
        exit 0
        ;;
    *)
        echo "  [FAIL] script did not print PASS"
        cat "$ACTUAL"
        exit 1
        ;;
esac
