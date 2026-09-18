#!/bin/sh
# Issue #344 regression: host caller-info channel.
#
# Two complementary tests:
#  1. caller_info_inproc.ae — Aether-side accessors for identity +
#     deadline, sequential-set/clear semantics, NULL identity behaviour.
#  2. caller_info_attrs.c — C-host attribute-pair flow, including
#     replacement, byte-budget overflow, attr-count overflow.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
LIB="$ROOT/build/libaether.a"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

# --- Aether-side test ---
ACTUAL="$TMPDIR/inproc.log"
if ! AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/caller_info_inproc.ae" \
        > "$ACTUAL" 2>"$TMPDIR/inproc.err"; then
    echo "  [FAIL] caller_info_inproc ae run exited non-zero"
    cat "$TMPDIR/inproc.err" | head -10
    exit 1
fi
if [ "$(tail -1 "$ACTUAL")" != "PASS" ]; then
    echo "  [FAIL] caller_info_inproc did not print PASS"
    cat "$ACTUAL"
    exit 1
fi

# --- C-side attribute test ---
# pthread is required because libaether.a uses TLS + the actor scheduler.
# -lm covers math.h symbols pulled by stdlib runtime functions.
if ! "${CC:-cc}" -O0 -g -I"$ROOT/runtime" \
        "$SCRIPT_DIR/caller_info_attrs.c" "$LIB" \
        -lpthread -lm -o "$TMPDIR/caller_info_attrs" \
        2>"$TMPDIR/cc.err"; then
    echo "  [FAIL] caller_info_attrs build failed"
    cat "$TMPDIR/cc.err" | head -10
    exit 1
fi

if ! "$TMPDIR/caller_info_attrs" 2>"$TMPDIR/attrs.err"; then
    echo "  [FAIL] caller_info_attrs runtime"
    cat "$TMPDIR/attrs.err" | head -20
    exit 1
fi
if ! grep -q '^PASS$' "$TMPDIR/attrs.err"; then
    echo "  [FAIL] caller_info_attrs did not print PASS"
    cat "$TMPDIR/attrs.err"
    exit 1
fi

echo "  [PASS] host caller-info channel (issue #344)"
