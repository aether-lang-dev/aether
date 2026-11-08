#!/bin/sh
# std.ipc bash-c-chain regression test (porter aeb-Claude's nit 1).
#
# Verifies fd 3 + AETHER_IPC_FD survive `bash -c '<wrapper>'`
# between the parent and child. aeb's driver_test wraps drivers
# via this same shape; without this test, a future bash regression
# would silently break the integration.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

# The platform from uname, never from the environment's OS, which an MSYS2
# login shell started from another shell does not carry (check_platform_env.py).
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*)
        echo "  [SKIP] std_ipc_bash_chain: std.ipc is POSIX-only in v1"
        exit 0 ;;
esac

if [ ! -x "$AE" ]; then
    echo "  [SKIP] std_ipc_bash_chain: ae not built"
    exit 0
fi

# bash is /bin/bash on Linux and macOS, /usr/local/bin/bash on FreeBSD
# (a package). The probe runs the one named here.
STD_IPC_BASH="$(command -v bash 2>/dev/null)"
case "$STD_IPC_BASH" in
    /*) ;;
    *) echo "  [SKIP] std_ipc_bash_chain: bash not available"
       exit 0 ;;
esac
export STD_IPC_BASH

cd "$ROOT" || exit 1

# The probe builds its child in the temp directory under a name of its own
# process (#2771); TMPDIR names this run's directory, which the trap removes.
TMPDIR="$(mktemp -d)"
export TMPDIR
trap 'rm -rf "$TMPDIR" || true' EXIT

if ! AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/probe.ae" >"$TMPDIR/out.log" 2>&1; then
    echo "  [FAIL] std_ipc_bash_chain"
    tail -40 "$TMPDIR/out.log" | sed 's/^/    /'
    exit 1
fi

if ! grep -q "std_ipc_bash_chain: 1 passing, 0 failing" "$TMPDIR/out.log"; then
    echo "  [FAIL] std_ipc_bash_chain — case did not pass"
    tail -40 "$TMPDIR/out.log" | sed 's/^/    /'
    exit 1
fi

echo "  [PASS] std_ipc_bash_chain"
exit 0
