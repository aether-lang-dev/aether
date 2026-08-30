#!/bin/sh
# wss:// non-blocking recv: the TLS-buffering case the ws:// test cannot reach.
#
# OpenSSL decrypts a whole TLS record at a time. When the peer sends two
# frames back-to-back they usually travel in one record, so after the client
# reads the first, the second is sitting DECRYPTED inside the SSL object with
# nothing pending on the socket. A readiness check that only polls the file
# descriptor answers "not ready" there, and a caller that believed it would
# block on a frame it already holds.
#
# That is the SSL_pending layer of ws_ready_now. Mutation-testing the ws://
# test showed it does not cover that layer -- deleting SSL_pending left it
# green -- which is exactly why this second test exists rather than being
# folded into the first.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
PORT=18252

[ -x "$AE" ] || { echo "  [SKIP] wss_client_nonblocking: ae not built"; exit 0; }

# The peer is python-websockets over TLS. Probe for a USABLE module rather
# than an importable one: Ubuntu's python3-websockets 9.1 imports fine and
# then dies on Python 3.10+ (it passes a removed loop= to asyncio.Lock).
PROBE=0
python3 "$ROOT/tests/integration/ws_probe_websockets.py" >/dev/null 2>&1 || PROBE=$?
if [ "$PROBE" != "0" ]; then
    if [ -n "$CI" ] && [ "$(uname -s)" = "Linux" ]; then
        echo "  [FAIL] no usable python websockets on a Linux CI runner (probe=$PROBE)."
        echo "         Install: pip install websockets"
        exit 1
    fi
    echo "  [SKIP] no usable python websockets module (pip install websockets)"
    exit 0
fi

command -v openssl >/dev/null 2>&1 || { echo "  [SKIP] wss_client_nonblocking: no openssl"; exit 0; }

TMP="$(mktemp -d)"
PEER_PID=""
cleanup() {
    # `|| :` per line: under `set -e` a failure inside a trap abandons the
    # rest of the handler, leaking $TMP and leaving a non-zero exit behind an
    # already-printed [PASS].
    if [ -n "$PEER_PID" ]; then kill "$PEER_PID" 2>/dev/null || :; fi
    rm -rf "$TMP" || :
    return 0
}
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

# MSYS2 rewrites POSIX-looking arguments, turning -subj "/CN=localhost" into a
# Windows path. Excluding just '/CN=' is deliberate: '*' would also stop the
# -keyout/-out conversion and openssl could then not open those paths.
MSYS2_ARG_CONV_EXCL='/CN=' \
openssl req -x509 -newkey rsa:2048 -keyout "$TMP/key.pem" -out "$TMP/cert.pem" \
    -days 2 -nodes -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" >"$TMP/ssl.log" 2>&1 \
    || { sed -n '1,10p' "$TMP/ssl.log"; fail "could not generate the localhost cert"; }

"$AE" build "$SCRIPT_DIR/wss_client.ae" -o "$TMP/cli" >"$TMP/b.log" 2>&1 \
    || { sed -n '1,10p' "$TMP/b.log"; fail "client did not build"; }

python3 "$SCRIPT_DIR/peer_tls_burst.py" "$PORT" "$TMP/cert.pem" "$TMP/key.pem" \
    > "$TMP/peer.log" 2>&1 &
PEER_PID=$!

i=0
while [ "$i" -lt 100 ]; do
    grep -q READY "$TMP/peer.log" 2>/dev/null && break
    sleep 0.1
    i=$((i + 1))
done
grep -q READY "$TMP/peer.log" 2>/dev/null || {
    sed -n '1,10p' "$TMP/peer.log"
    fail "TLS peer never became READY"
}

if command -v timeout >/dev/null 2>&1; then TIMEOUT="timeout 60"
elif command -v gtimeout >/dev/null 2>&1; then TIMEOUT="gtimeout 60"
else TIMEOUT=""; fi

# The self-signed cert doubles as its own CA.
OUT=$(SSL_CERT_FILE="$TMP/cert.pem" $TIMEOUT "$TMP/cli" "$PORT" 2>&1) || {
    echo "$OUT" | sed 's/^/         /'
    fail "wss client exited non-zero"
}
case "$OUT" in
    *"PASS: wss non-blocking recv / poll"*) ;;
    *) echo "$OUT" | sed 's/^/         /'; fail "wss client did not report PASS" ;;
esac

echo "  [PASS] wss_client_nonblocking: TLS-buffered frame still visible to ws_poll"
