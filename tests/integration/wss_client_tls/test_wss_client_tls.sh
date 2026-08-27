#!/bin/sh
# wss:// -- the WebSocket client over TLS.
#
# Three cases, because a test that only proved the happy path would pass
# equally well with certificate verification switched off entirely:
#
#   1. trusted CA, right host   -> round-trip succeeds
#   2. self-signed, untrusted   -> refused
#   3. trusted CA, WRONG host   -> refused
#
# Case 3 is the one that catches a missing hostname pin specifically: the
# chain validates, so only X509_VERIFY_PARAM_set1_host rejects it. Without
# that call case 3 passes and the bug ships.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
PORT_OK=18192
PORT_BAD=18193

[ -x "$AE" ] || { echo "  [SKIP] wss_client_tls: ae not built"; exit 0; }

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

command -v openssl >/dev/null 2>&1 || { echo "  [SKIP] wss_client_tls: no openssl"; exit 0; }

TMP="$(mktemp -d)"
OK_PID=""
BAD_PID=""
cleanup() {
    # Every step tolerates its own failure: under `set -e` a failing command
    # in a trap abandons the rest of the handler, which would leak $TMP and
    # leave a non-zero status behind a test that had already passed.
    if [ -n "$OK_PID" ];  then kill "$OK_PID"  2>/dev/null || :; fi
    if [ -n "$BAD_PID" ]; then kill "$BAD_PID" 2>/dev/null || :; fi
    rm -rf "$TMP" || :
    return 0
}
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

# A cert for localhost, and one that is valid only for a host we will not ask
# for. Both are self-signed, so each doubles as its own CA.
openssl req -x509 -newkey rsa:2048 -keyout "$TMP/key.pem" -out "$TMP/cert.pem" \
    -days 2 -nodes -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" >"$TMP/ssl.log" 2>&1 \
    || { sed -n '1,10p' "$TMP/ssl.log"; fail "could not generate the localhost cert"; }
openssl req -x509 -newkey rsa:2048 -keyout "$TMP/wrong.key" -out "$TMP/wrong.pem" \
    -days 2 -nodes -subj "/CN=wrong.example" \
    -addext "subjectAltName=DNS:wrong.example" >>"$TMP/ssl.log" 2>&1 \
    || { sed -n '1,10p' "$TMP/ssl.log"; fail "could not generate the wrong-host cert"; }

"$AE" build "$SCRIPT_DIR/wss_client.ae" -o "$TMP/cli" >"$TMP/b.log" 2>&1 \
    || { sed -n '1,15p' "$TMP/b.log"; fail "client did not build"; }

start_peer() {   # $1=port $2=cert $3=key $4=logfile -> echoes the pid
    python3 "$SCRIPT_DIR/peer_tls.py" "$1" "$2" "$3" > "$4" 2>&1 &
    echo $!
}
wait_ready() {   # $1=logfile $2=label
    i=0
    while [ "$i" -lt 100 ]; do
        grep -q READY "$1" 2>/dev/null && return 0
        sleep 0.1
        i=$((i + 1))
    done
    sed -n '1,10p' "$1"
    fail "$2 never became READY"
}

OK_PID=$(start_peer "$PORT_OK" "$TMP/cert.pem" "$TMP/key.pem" "$TMP/ok.log")
wait_ready "$TMP/ok.log" "TLS peer (localhost cert)"
BAD_PID=$(start_peer "$PORT_BAD" "$TMP/wrong.pem" "$TMP/wrong.key" "$TMP/bad.log")
wait_ready "$TMP/bad.log" "TLS peer (wrong-host cert)"

# 1. Trusted CA + right host -> must succeed.
OUT=$(SSL_CERT_FILE="$TMP/cert.pem" "$TMP/cli" round-trip "$PORT_OK" 2>&1) || {
    echo "$OUT" | sed 's/^/         /'
    fail "wss round-trip failed against a trusted peer"
}
case "$OUT" in
    *"PASS: wss round-trip"*) ;;
    *) echo "$OUT" | sed 's/^/         /'; fail "round-trip did not report PASS" ;;
esac

# 2. Untrusted self-signed -> must be refused. No SSL_CERT_FILE, so the cert
#    is not an anchor; the system store will not contain a 2-day test cert.
OUT=$("$TMP/cli" expect-reject "$PORT_OK" 2>&1) || {
    echo "$OUT" | sed 's/^/         /'
    fail "untrusted self-signed cert was not refused"
}
case "$OUT" in
    *"PASS: rejected as expected"*) ;;
    *) echo "$OUT" | sed 's/^/         /'; fail "case 2 did not refuse" ;;
esac

# 3. Trusted CA but the cert is for wrong.example -> must be refused on the
#    hostname, not the chain.
OUT=$(SSL_CERT_FILE="$TMP/wrong.pem" "$TMP/cli" expect-reject "$PORT_BAD" 2>&1) || {
    echo "$OUT" | sed 's/^/         /'
    fail "a cert for the wrong host was accepted"
}
case "$OUT" in
    *"PASS: rejected as expected"*) ;;
    *) echo "$OUT" | sed 's/^/         /'; fail "case 3 did not refuse" ;;
esac

echo "  [PASS] wss_client_tls: TLS round-trip; untrusted and wrong-host certs refused"
