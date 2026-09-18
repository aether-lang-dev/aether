#!/bin/sh
# Integration: actor-message round-trip with a *StringSeq field.
#
# Sender splits a CSV into a cons-cell *StringSeq via
# string.split_to_seq, sends it as a message field, receiver
# pattern-matches with [] / [h|t] and prints each element. End-state
# is "ok" on stdout and exit 0.
#
# This exercises the full chain: typechecker accepting the seq field
# type, codegen emitting the *StringSeq across the actor mailbox
# boundary, runtime delivery of the cons-cell pointer, receiver
# match-arm dispatch on `*StringSeq` matched expression.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

ACTUAL="$TMPDIR/actual.txt"
if ! AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/repro.ae" >"$ACTUAL" 2>"$TMPDIR/err.log"; then
    echo "  [FAIL] ae run exited non-zero"
    head -30 "$TMPDIR/err.log"
    cat "$ACTUAL"
    exit 1
fi

# Receiver should print three sites in order, then "ok".
expected_lines="alpha.example.com beta.example.com gamma.example.com ok"
for line in $expected_lines; do
    if ! grep -qx "$line" "$ACTUAL"; then
        echo "  [FAIL] expected '$line' in stdout"
        echo "--- stdout ---"
        cat "$ACTUAL"
        exit 1
    fi
done

echo "  [PASS] string_seq_in_message: cons-cell *StringSeq round-trips through an actor message"
