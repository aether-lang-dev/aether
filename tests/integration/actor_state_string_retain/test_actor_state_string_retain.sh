#!/bin/sh
# Regression: a string carried in as a MESSAGE FIELD and retained into
# ACTOR STATE survives past the originating message's release.
#
# The message's string field is freed by <Msg>_release_fields immediately
# after the handler returns, so `self->field = <message-string>` stored a
# raw pointer that dangled — a LATER message printed freed bytes (the aeo
# actor-state-string corruption). The compiler now copies a borrowed
# string into an owned AetherString on the retain (freeing any prior
# copy). This also covers the `string.concat(in, "")` defensive-copy
# idiom, which previously failed to COMPILE (`'_heap_n' undeclared`)
# because actor handlers skipped the function-scope heap-string hoist.
#
# Pass: prints the two expected lines with the retained values intact.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

ACTUAL="$TMPDIR/actual.txt"
if ! AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/repro.ae" >"$ACTUAL" 2>"$TMPDIR/err.log"; then
    echo "  [FAIL] ae run exited non-zero — actor-state-string regression?"
    head -30 "$TMPDIR/err.log"
    exit 1
fi

if ! grep -q '^n=aeo-db c=cfg!$' "$ACTUAL"; then
    echo "  [FAIL] expected 'n=aeo-db c=cfg!' (retained message strings intact)"
    echo "--- stdout ---"; cat "$ACTUAL"
    exit 1
fi
if ! grep -q '^n=second-longer-value c=cfg!$' "$ACTUAL"; then
    echo "  [FAIL] expected 'n=second-longer-value c=cfg!' after reassignment"
    echo "--- stdout ---"; cat "$ACTUAL"
    exit 1
fi

echo "  [PASS] actor_state_string_retain: message string retained into state survives"
