#!/bin/sh
# Regression: os.* spawn paths reset the child's signal state before exec, so a
# child does NOT inherit the parent's BLOCKED signal mask (nor its SIG_IGN
# dispositions). Without the reset, an embedder that masks signals (JVM, .NET,
# Ruby, Julia) spawns children deaf to os.kill's signals — os.kill(token,
# SIGTERM) is delivered but ignored and an unbounded os.wait hangs forever.
# asks/spawn-child-inherits-blocked-signal-mask.md.
#
# The bug needs a parent with a blocked mask, which Aether cannot set from the
# language, so this is a C harness: it blocks SIGTERM/INT/QUIT, then drives the
# real os_run_capture_raw on a probe that prints its own SigBlk. The child's
# mask must be all-zero.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

# /proc/self/status is Linux-specific, and the fix is a POSIX fork/exec path;
# skip cleanly elsewhere (Windows has no fork; other Unixes lack /proc SigBlk).
case "$(uname -s)" in
    Linux) ;;
    *) echo "  [SKIP] spawn signal-mask reset: Linux-only (needs /proc SigBlk)"; exit 0 ;;
esac

if [ ! -f "$ROOT/build/libaether.a" ]; then
    echo "  [SKIP] spawn signal-mask reset: build/libaether.a not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

if ! gcc -o "$tmpdir/harness" "$SCRIPT_DIR/harness.c" \
        "$ROOT/build/libaether.a" -lpthread -ldl -lm 2>"$tmpdir/gcc.err"; then
    echo "  [FAIL] could not link the harness against libaether.a"
    sed 's/^/          /' "$tmpdir/gcc.err" | head -8
    exit 1
fi

out="$("$tmpdir/harness" "$SCRIPT_DIR/probe.sh" 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && printf '%s' "$out" | grep -q "PASS: spawned child has a clear signal mask"; then
    echo "  [PASS] spawned child's signal mask is clear despite a masking parent"
    echo "PASS: spawn_signal_mask_reset"
    exit 0
fi
echo "  [FAIL] spawned child inherited the parent's blocked mask"
printf '%s\n' "$out" | sed 's/^/          /' | head -6
echo "FAIL: spawn_signal_mask_reset"
exit 1
