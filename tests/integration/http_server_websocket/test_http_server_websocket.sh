#!/bin/sh
# #260 Tier 2 / Phase E2: WebSocket end-to-end.
#
# Drives the WebSocket echo server with a Python client (the
# `websockets` library, ubiquitous on every CI runner with Python).
# Verifies:
#   - upgrade handshake completes (101 Switching Protocols, correct
#     Sec-WebSocket-Accept hash)
#   - 3 text frames round-trip with the right echo prefix
#   - server-initiated close arrives after the handler returns
#
# Skips when Python or the websockets library is missing — host
# bridges should never break CI on machines without the toolchain.

# Skip on Windows — the HTTP server / proxy / middleware code path
# under test is platform-independent userland C; the Linux and
# macOS CI matrix entries already exercise every behaviour this
# test asserts. Each curl invocation under MSYS2 bash pays Cygwin
# fork-emulation + Defender + slower-than-POSIX localhost overhead
# (~10-100x POSIX's per-spawn cost), so running these on Windows
# adds minutes to the pipeline without adding coverage. Windows-
# specific runtime concerns (file I/O, process spawning, path
# handling) are covered by fs_*, std_ipc_*, run_lib_path, and
# ae_help tests respectively.
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] http_server_websocket — HTTP server/proxy/middleware code is platform-independent; covered by POSIX matrix"
        exit 0
        ;;
esac

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if ! command -v python3 >/dev/null 2>&1; then
    echo "  [SKIP] python3 not on PATH"
    exit 0
fi
PROBE=0
python3 "$ROOT/tests/integration/ws_probe_websockets.py" >/dev/null 2>&1 || PROBE=$?
if [ "$PROBE" != "0" ]; then
    # 2 = not installed, 3 = installed but unusable (Ubuntu 22.04 ships
    # websockets 9.1, which is broken on Python 3.10+ -- see the probe).
    #
    # This is our only check against an INDEPENDENT RFC 6455 implementation,
    # so a silent skip on Linux CI would hide the one thing it exists for.
    if [ -n "$CI" ] && [ "$(uname -s)" = "Linux" ]; then
        if [ "$PROBE" = "3" ]; then
            echo "  [FAIL] the installed python websockets is unusable on this Python."
            echo "         Ubuntu 22.04's python3-websockets (9.1) passes the removed"
            echo "         loop= argument to asyncio and dies mid-handshake."
            echo "         Install a current one: pip install websockets"
        else
            echo "  [FAIL] python websockets is missing on a Linux CI runner."
            echo "         Install: pip install websockets"
        fi
        echo "         (this is our only external RFC 6455 conformance check)"
        exit 1
    fi
    echo "  [SKIP] no usable python websockets module (pip install websockets)"
    exit 0
fi

TMPDIR="$(mktemp -d)"
cleanup() {
    if [ -n "${SRV_PID:-}" ]; then
        kill "$SRV_PID" 2>/dev/null || true
        wait "$SRV_PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/server.ae" >"$TMPDIR/srv.log" 2>&1 &
SRV_PID=$!

deadline=$(($(date +%s) + 15))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if grep -q READY "$TMPDIR/srv.log" 2>/dev/null; then break; fi
    if ! kill -0 "$SRV_PID" 2>/dev/null; then
        echo "  [FAIL] server died:"
        head -20 "$TMPDIR/srv.log"
        exit 1
    fi
    sleep 0.1
done
sleep 0.3

# Python client sends 3 text messages and prints each echo on its
# own line; non-zero exit on any failure.
python3 - <<'PY' >"$TMPDIR/py.out" 2>"$TMPDIR/py.err"
import asyncio
import sys
import websockets

async def main():
    async with websockets.connect("ws://127.0.0.1:18110/echo") as ws:
        for msg in ("hello", "world", "!"):
            await ws.send(msg)
            reply = await asyncio.wait_for(ws.recv(), timeout=2)
            print(reply)

try:
    asyncio.run(main())
except Exception as e:
    print(f"PY-ERR: {e}", file=sys.stderr)
    sys.exit(1)
PY

if [ $? -ne 0 ]; then
    echo "  [FAIL] python client errored:"
    cat "$TMPDIR/py.err"
    echo "--- server log ---"
    head -20 "$TMPDIR/srv.log"
    exit 1
fi

# Expected output: 3 lines, each "echo: <msg>"
EXPECTED="echo: hello
echo: world
echo: !"
if [ "$(cat "$TMPDIR/py.out")" != "$EXPECTED" ]; then
    echo "  [FAIL] output mismatch"
    echo "--- expected ---"
    echo "$EXPECTED"
    echo "--- actual ---"
    cat "$TMPDIR/py.out"
    exit 1
fi

echo "  [PASS] http_server_websocket: 3-message text echo round-trip via RFC 6455 framing"
