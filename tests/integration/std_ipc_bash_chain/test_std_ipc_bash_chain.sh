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

if [ "$OS" = "Windows_NT" ]; then
    echo "  [SKIP] std_ipc_bash_chain: std.ipc is POSIX-only in v1"
    exit 0
fi

if [ ! -x "$AE" ]; then
    echo "  [SKIP] std_ipc_bash_chain: ae not built"
    exit 0
fi

if [ ! -x /bin/bash ]; then
    echo "  [SKIP] std_ipc_bash_chain: /bin/bash not available"
    exit 0
fi

cd "$ROOT" || exit 1

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true; rm -f /tmp/std_ipc_bash_chain_child || true' EXIT

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
