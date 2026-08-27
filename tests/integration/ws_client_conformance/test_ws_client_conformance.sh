#!/bin/sh
# WebSocket CLIENT conformance against an INDEPENDENT RFC 6455 implementation.
#
# This is the mirror of tests/integration/http_server_websocket (their client,
# our server). Here the peer is a python3-websockets SERVER and the client
# under test is ours.
#
# Why both directions matter: tests/integration/ws_client_loopback dials our
# own server, so an error both ends make together is invisible to it -- if we
# masked outbound frames the wrong way AND unmasked them the same wrong way,
# the echo still round-trips. A third-party peer has no such symmetry: the
# websockets library enforces RFC 6455 s5.1 and closes on an unmasked or
# mis-masked client frame, so this test fails where the loopback one passes.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
PORT=18146

[ -x "$AE" ] || { echo "  [SKIP] ws_client_conformance: ae not built"; exit 0; }

if ! python3 -c "import websockets" 2>/dev/null; then
    # Same policy as http_server_websocket: this is external validation, so a
    # silent skip on Linux CI would hide the one thing the test exists for.
    if [ -n "$CI" ] && [ "$(uname -s)" = "Linux" ]; then
        echo "  [FAIL] python3-websockets is missing on a Linux CI runner."
        echo "         Install: sudo apt-get install -y python3-websockets"
        exit 1
    fi
    echo "  [SKIP] websockets module not installed"
    echo "         Debian/Ubuntu: sudo apt-get install python3-websockets (370KB)"
    echo "         other: pip install websockets"
    exit 0
fi

TMP="$(mktemp -d)"
SRV_PID=""
cleanup() {
    [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null
    rm -rf "$TMP"
    return 0
}
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

"$AE" build "$SCRIPT_DIR/ws_conformance.ae" -o "$TMP/wscli" >"$TMP/b.log" 2>&1 \
    || { sed -n '1,10p' "$TMP/b.log"; fail "client did not build"; }

python3 "$SCRIPT_DIR/peer.py" "$PORT" > "$TMP/srv.log" 2>&1 &
SRV_PID=$!

i=0
while [ "$i" -lt 100 ]; do
    grep -q READY "$TMP/srv.log" 2>/dev/null && break
    sleep 0.1
    i=$((i + 1))
done
grep -q READY "$TMP/srv.log" 2>/dev/null || {
    sed -n '1,10p' "$TMP/srv.log"
    fail "python websockets peer never became READY"
}

OUT=$(timeout 30 "$TMP/wscli" 2>&1) || {
    echo "$OUT" | sed 's/^/         /'
    sed -n '1,10p' "$TMP/srv.log" | sed 's/^/    srv: /'
    fail "client exited non-zero"
}
case "$OUT" in
    *"PASS: client conformance"*) ;;
    *) echo "$OUT" | sed 's/^/         /'; fail "conformance round-trip did not report PASS" ;;
esac

echo "  [PASS] ws_client_conformance: 3-message round-trip vs python3-websockets"
