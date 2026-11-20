#!/bin/sh
# A child's exit status with SIGCHLD ignored by the process (POSIX).
#
# An ignored SIGCHLD (SIG_IGN or SA_NOCLDWAIT) is inherited across exec,
# and the kernel then reaps each child as it exits: waitpid fails with
# ECHILD. A program the Erlang VM starts has it so. os.run_capture read the
# child's output and then returned (stdout, -1, "waitpid failed"), os.run
# returned -1, and so did every other call that waits. probe.ae runs each
# of them; this script runs the same binary as it is and under launch.c,
# which execs it with SIGCHLD ignored. Both runs must pass.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] os_sigchld_ignored: no SIGCHLD on Windows"
        exit 0 ;;
esac
if [ ! -x "$AE" ]; then
    echo "  [SKIP] os_sigchld_ignored: $AE not built"
    exit 0
fi
CC_BIN="${CC:-cc}"
if ! command -v "$CC_BIN" >/dev/null 2>&1; then
    echo "  [SKIP] os_sigchld_ignored: no C compiler for the launcher"
    exit 0
fi

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

if ! AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/probe.ae" -o "$TMPDIR/probe" \
        >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] os_sigchld_ignored: build failed"
    sed 's/^/    /' "$TMPDIR/build.log" | head -15
    exit 1
fi
if ! "$CC_BIN" -o "$TMPDIR/launch" "$SCRIPT_DIR/launch.c" >"$TMPDIR/cc.log" 2>&1; then
    echo "  [FAIL] os_sigchld_ignored: launcher did not build"
    sed 's/^/    /' "$TMPDIR/cc.log" | head -15
    exit 1
fi

fail=0
if ! "$TMPDIR/probe" >"$TMPDIR/plain.log" 2>&1; then
    echo "  [FAIL] os_sigchld_ignored: SIGCHLD at its default"
    sed 's/^/    /' "$TMPDIR/plain.log" | grep -v '^    ok ' | head -30
    fail=1
fi
if ! "$TMPDIR/launch" "$TMPDIR/probe" >"$TMPDIR/ignored.log" 2>&1; then
    echo "  [FAIL] os_sigchld_ignored: SIGCHLD ignored"
    sed 's/^/    /' "$TMPDIR/ignored.log" | grep -v '^    ok ' | head -30
    fail=1
fi
[ "$fail" -eq 0 ] || exit 1
echo "  [PASS] os_sigchld_ignored (default and ignored SIGCHLD)"
