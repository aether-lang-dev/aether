#!/bin/sh
# Phase A2.1 / issue #265 prerequisite: default function arguments.
# Asserts the parser accepts `param: type = expr`, the typechecker
# fills missing trailing args from the declared defaults, and codegen
# emits valid C for every shape.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

ACTUAL="$TMPDIR/actual.txt"
if ! AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/uses_defaults.ae" >"$ACTUAL" 2>"$TMPDIR/err.log"; then
    echo "  [FAIL] ae run exited non-zero"
    cat "$TMPDIR/err.log" | head -10
    exit 1
fi

EXPECTED='Hello, Ada!
Hi, Ada!
50
100
140
>>> ok!
>>> go***'

if [ "$(cat "$ACTUAL")" != "$EXPECTED" ]; then
    echo "  [FAIL] output mismatch"
    echo "--- expected ---"
    echo "$EXPECTED"
    echo "--- actual ---"
    cat "$ACTUAL"
    exit 1
fi

echo "  [PASS] default_arguments: parser + typechecker + codegen across 6 cases"
