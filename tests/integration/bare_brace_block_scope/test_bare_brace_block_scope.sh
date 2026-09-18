#!/bin/sh
# Integration regression for #286.
#
# Pre-fix: a bare-brace `{ ... }` block on the line after a
# function-call RHS was eaten as that call's trailing closure.
# References to the outer-scope variable from inside the
# synthesised closure body produced misleading "Undefined variable"
# diagnostics — the same shape as the 360-line failing file from
# the downstream svn-aether port.
#
# Post-fix: trailing-closure consumption requires `{` on the same
# line as the call's closing `)`. A `{` on a later line is parsed
# as an independent bare-brace block and sees outer locals via the
# ordinary lexical-scope chain.
#
# Pass: program compiles cleanly and prints "ok" on its own line.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

ACTUAL="$TMPDIR/actual.txt"
if ! AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/repro.ae" >"$ACTUAL" 2>"$TMPDIR/err.log"; then
    echo "  [FAIL] ae run exited non-zero — bare-brace-after-call regression?"
    head -30 "$TMPDIR/err.log"
    exit 1
fi

if ! grep -q '^ok$' "$ACTUAL"; then
    echo "  [FAIL] expected line 'ok' in stdout"
    echo "--- stdout ---"
    cat "$ACTUAL"
    echo "--- stderr ---"
    head -20 "$TMPDIR/err.log"
    exit 1
fi

echo "  [PASS] bare_brace_block_scope: nested bare-brace block sees outer locals (#286)"
