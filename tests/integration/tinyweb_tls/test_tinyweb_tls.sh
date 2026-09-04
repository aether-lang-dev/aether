#!/bin/sh
# tinyweb over TLS (`with_tls`).
#
# tinyweb wraps std.http's server, which has had server_set_tls all along --
# tinyweb simply never surfaced it, so every tinyweb app was plaintext-only.
# `with_tls` records a PEM pair on the config map and tw_start applies it
# before registering routes.
#
# Asserts:
#   - a real TLS handshake and the route's body over https
#   - plain http to the TLS port does NOT return the body
#   - a HALF-configured pair (cert, no key) REFUSES to start, rather than
#     silently serving plaintext on a port the caller believes is encrypted --
#     which is the failure mode worth guarding
#
# Skips on Windows, as the other HTTP-server suites do: this is
# platform-independent userland C and each curl under MSYS2 costs 10-100x a
# POSIX spawn.
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] tinyweb_tls — HTTP server code is platform-independent; covered by POSIX matrix"
        exit 0
        ;;
esac

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
command -v curl >/dev/null 2>&1 || { echo "  [SKIP] tinyweb_tls: curl not on PATH"; exit 0; }
command -v openssl >/dev/null 2>&1 || { echo "  [SKIP] tinyweb_tls: openssl not on PATH"; exit 0; }

TMPDIR="$(mktemp -d)"
cleanup() {
    if [ -n "${SRV_PID:-}" ]; then
        kill "$SRV_PID" 2>/dev/null || true
        wait "$SRV_PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

openssl req -x509 -newkey rsa:2048 -keyout "$TMPDIR/k.pem" -out "$TMPDIR/c.pem" \
    -days 2 -nodes -subj "/CN=127.0.0.1" -addext "subjectAltName=IP:127.0.0.1" \
    >/dev/null 2>&1 || { echo "  [SKIP] tinyweb_tls: could not generate a test cert"; exit 0; }

export TW_TLS_CERT="$TMPDIR/c.pem"
export TW_TLS_KEY="$TMPDIR/k.pem"
PORT=18845

AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/server.ae" --lib "$ROOT/contrib" \
    >"$TMPDIR/srv.log" 2>&1 &
SRV_PID=$!

# Probe the port rather than trusting READY -- the listener opens after it.
deadline=$(($(date +%s) + 20))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if ! kill -0 "$SRV_PID" 2>/dev/null; then
        echo "  [FAIL] tinyweb_tls: server died:"; head -20 "$TMPDIR/srv.log"; exit 1
    fi
    curl -sk -o /dev/null --max-time 1 "https://127.0.0.1:$PORT/hello" 2>/dev/null && break
    sleep 0.2
done

# --- 1. the body, over a real TLS handshake ----------------------------
BODY=$(curl -sk --max-time 6 "https://127.0.0.1:$PORT/hello" 2>/dev/null || echo "")
[ "$BODY" = "secure hello" ] || {
    echo "  [FAIL] tinyweb_tls: https body was '$BODY', want 'secure hello'"
    head -10 "$TMPDIR/srv.log" | sed 's/^/    /'; exit 1; }

# Confirm it really negotiated TLS rather than happening to answer.
curl -skv --max-time 6 "https://127.0.0.1:$PORT/hello" 2>&1 \
    | grep -qiE "TLS handshake|SSL connection|TLSv1" || {
    echo "  [FAIL] tinyweb_tls: no TLS handshake in the curl trace"; exit 1; }

# --- 2. plain http to the TLS port must not serve the body -------------
PLAIN=$(curl -s --max-time 4 "http://127.0.0.1:$PORT/hello" 2>/dev/null || echo "")
case "$PLAIN" in
    *"secure hello"*)
        echo "  [FAIL] tinyweb_tls: plaintext http returned the body on a TLS port"; exit 1 ;;
esac

# --- 3. a half-configured pair must REFUSE to start --------------------
HALF=$(AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/half.ae" --lib "$ROOT/contrib" 2>&1 || true)
case "$HALF" in
    *"needs BOTH a cert and a key"*) ;;
    *)
        echo "  [FAIL] tinyweb_tls: a cert without a key did not refuse to start"
        echo "$HALF" | sed 's/^/    /' | head -6; exit 1 ;;
esac

echo "  [PASS] tinyweb_tls: serves https, refuses plaintext, refuses a half-configured pair"
