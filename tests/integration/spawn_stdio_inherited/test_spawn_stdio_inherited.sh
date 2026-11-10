#!/bin/sh
# Regression: os.* spawn paths give the child its parent's standard streams
# even when the parent marked them close-on-exec (Node does), a spawn from
# a parent with 0 and 2 closed returns once the child starts (#3170), and
# run_full and spawn_proc_to wire their child's stdio with any of the
# parent's 0, 1 and 2 closed (#3199), and run_pipe's child gets
# AETHER_IPC_FD=3 with no setenv between fork and exec (#3200). See
# harness.c.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/syslibs.sh"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] spawn stdio inherited: POSIX fork/exec only"; exit 0 ;;
esac

if [ ! -f "$ROOT/build/libaether.a" ]; then
    echo "  [SKIP] spawn stdio inherited: build/libaether.a not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

if ! ${CC:-cc} -o "$tmpdir/harness" "$SCRIPT_DIR/harness.c" \
        "$ROOT/build/libaether.a" -lpthread -lm 2>"$tmpdir/cc.err"; then
    # The runtime links more libraries on some platforms; try the full set.
    if ! ${CC:-cc} -o "$tmpdir/harness" "$SCRIPT_DIR/harness.c" \
            "$ROOT/build/libaether.a" -lpthread $AE_DL_LIBS -lm -lz -lssl -lcrypto 2>"$tmpdir/cc.err"; then
        echo "  [FAIL] could not link the harness against libaether.a"
        sed 's/^/          /' "$tmpdir/cc.err" | head -8
        exit 1
    fi
fi

out="$("$tmpdir/harness" "$SCRIPT_DIR/probe.sh" "$tmpdir" 2>&1 </dev/null)"
rc=$?
if [ "$rc" -eq 0 ] && printf '%s' "$out" | grep -q "PASS: the spawned child has stdin, stdout and stderr" \
        && printf '%s' "$out" | grep -q "PASS: a spawn with 0 and 2 closed returns once the child starts" \
        && printf '%s' "$out" | grep -q "PASS: run_full and spawn_proc_to wire their stdio" \
        && printf '%s' "$out" | grep -q "PASS: run_pipe's child sees AETHER_IPC_FD=3 once"; then
    echo "  [PASS] a spawned child has its standard streams although the parent marked them close-on-exec"
    echo "  [PASS] a spawn from a parent with 0 and 2 closed returns once the child starts"
    echo "  [PASS] run_full and spawn_proc_to wire their stdio with any of 0, 1 and 2 closed in the parent"
    echo "  [PASS] run_pipe's child gets AETHER_IPC_FD=3 once, with no setenv between fork and exec"
    echo "PASS: spawn_stdio_inherited"
    exit 0
fi
echo "  [FAIL] a spawned child lost its standard streams, a spawn waited for its child, or a child's stdio was miswired"
printf '%s\n' "$out" | sed 's/^/          /' | head -20
echo "FAIL: spawn_stdio_inherited"
exit 1
