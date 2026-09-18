#!/bin/sh
# Issue #347 regression: aether_panic prints a filtered stack trace
# to stderr before aborting when there's no try/catch frame. The
# trace is best-effort under -O2 (tail-call + inlining can collapse
# user frames), but the "Stack trace" header and at least one frame
# (typically `main`) must always appear; otherwise the runtime
# regressed back to the silent `panic outside any try/catch` line.
#
# Tests the codegen-side `panic("...")` path. Contract violations
# and signal-converted panics use the same fallback printer once
# they reach aether_panic, so this also exercises the runtime
# branch.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

BIN="$TMPDIR/panic_program"
STDERR="$TMPDIR/stderr.log"

if ! AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/panic_program.ae" -o "$BIN" >/dev/null 2>"$TMPDIR/build.log"; then
    echo "  [FAIL] ae build exited non-zero"
    cat "$TMPDIR/build.log" | head -10
    exit 1
fi

# Expected: process aborts with a non-zero exit code AND emits the
# trace markers on stderr.
if "$BIN" >/dev/null 2>"$STDERR"; then
    echo "  [FAIL] panic program returned 0 — should have aborted"
    cat "$STDERR" | head -10
    exit 1
fi

if ! grep -q "panic outside any try/catch" "$STDERR"; then
    echo "  [FAIL] missing the panic-reason line on stderr"
    cat "$STDERR" | head -10
    exit 1
fi

if ! grep -q "Stack trace" "$STDERR"; then
    echo "  [FAIL] missing 'Stack trace' header on stderr"
    cat "$STDERR" | head -10
    exit 1
fi

# At least one frame line. Frame lines are "  N: name". A regression
# that captured zero frames would still emit the header but no body,
# so we check the body explicitly.
if ! grep -qE '^  0:' "$STDERR"; then
    echo "  [FAIL] no frame entries under the trace header"
    cat "$STDERR" | head -10
    exit 1
fi

# AETHER_STACK_TRACE=0 must suppress the trace (used by tests that
# diff stderr line-for-line and don't want the noise).
SUPPRESSED="$TMPDIR/suppressed.log"
if AETHER_STACK_TRACE=0 "$BIN" >/dev/null 2>"$SUPPRESSED"; then
    echo "  [FAIL] panic program returned 0 under AETHER_STACK_TRACE=0"
    exit 1
fi

if grep -q "Stack trace" "$SUPPRESSED"; then
    echo "  [FAIL] AETHER_STACK_TRACE=0 did not suppress the trace"
    cat "$SUPPRESSED" | head -10
    exit 1
fi

echo "  [PASS] panic stack trace (issue #347)"
