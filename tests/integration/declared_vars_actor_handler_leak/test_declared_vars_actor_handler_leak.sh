#!/bin/sh
# Regression: declared_vars list leaked from a regular function's
# parameters into a subsequent actor receive handler.
#
# Before the fix, codegen_actor.c never reset gen->declared_vars at
# the top of each handler. If a free function defined earlier in the
# same module took a parameter named (e.g.) `loop_start` and the
# handler then first-assigned a local of the same name inside a
# `while` body, hoist_loop_vars saw the stale entry and skipped
# emitting `int64_t loop_start;` at the function scope. The handler
# body then assigned to an undeclared identifier and gcc/clang
# rejected the generated C with "use of undeclared identifier".
#
# Pass: program compiles cleanly and prints "ok".

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

ACTUAL="$TMPDIR/actual.txt"
if ! AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/repro.ae" >"$ACTUAL" 2>"$TMPDIR/err.log"; then
    echo "  [FAIL] ae run exited non-zero — declared_vars leak regression?"
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

echo "  [PASS] declared_vars_actor_handler_leak: handler compiles + runs cleanly"
