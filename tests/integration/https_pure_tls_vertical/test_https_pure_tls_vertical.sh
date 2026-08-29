#!/bin/sh
# HTTPS end to end with a pure-Aether TLS client: our std.http server on one
# side, std.cryptography.tls13_client on the other, one HTTP/1.1 exchange
# framed over the raw TLS stream.
#
# Why this exists. std.http.client's HTTPS path is OpenSSL
# (std/net/aether_http.c), so `ae build --target=` -- which links no OpenSSL --
# returns "HTTPS requested but the build has no OpenSSL support" at runtime.
# A downstream port read the tree, concluded HTTPS was impossible in a cross
# build, and filed asks/http-client-pure-tls-backend-for-crossbuild.md asking
# for a pure TLS backend to be written.
#
# Most of it already exists: tls13_client is a complete pure-Aether TLS 1.3
# client with full server authentication, and it cross-builds. What was
# missing was anything demonstrating that, which is what this test is. It
# pins the vertical slice so the claim is checkable rather than asserted, and
# gives the std.http.client integration a target to match.
#
# The client half deliberately uses NO OpenSSL. The server half still does
# (std/net/aether_http_server.c terminates TLS with SSL_accept), so this
# proves one direction; a pure server needs tls13_hs to gain a ClientHello
# parser and a ServerHello builder, which it does not have yet.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
PORT=18447

[ -x "$AE" ] || { echo "  [SKIP] https_pure_tls_vertical: ae not built"; exit 0; }
command -v openssl >/dev/null 2>&1 || { echo "  [SKIP] https_pure_tls_vertical: no openssl to make a cert"; exit 0; }

TMP="$(mktemp -d)"
SRV_PID=""
cleanup() {
    if [ -n "$SRV_PID" ]; then kill "$SRV_PID" 2>/dev/null || :; fi
    rm -rf "$TMP" || :
    return 0
}
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

# A self-signed cert for 127.0.0.1. The client authenticates the server for
# real -- chain, validity and hostname -- so the cert has to carry an IP SAN
# and be handed to the client as its trust anchor.
# MSYS2_ARG_CONV_EXCL: on MSYS2 the shell rewrites any argument that looks
# like a POSIX path, so `-subj "/CN=127.0.0.1"` reaches openssl as
# "D:/a/_temp/msys64/CN=127.0.0.1" and it refuses the subject. The variable is
# inert everywhere else -- verified on Linux, where the subject and both SANs
# come out identical with and without it.
MSYS2_ARG_CONV_EXCL='*' \
openssl req -x509 -newkey rsa:2048 -keyout "$TMP/key.pem" -out "$TMP/cert.pem" \
    -days 2 -nodes -subj "/CN=127.0.0.1" \
    -addext "subjectAltName=IP:127.0.0.1,DNS:localhost" >"$TMP/ssl.log" 2>&1 \
    || { sed -n '1,10p' "$TMP/ssl.log"; fail "could not generate a test certificate"; }

"$AE" build "$SCRIPT_DIR/server.ae" -o "$TMP/srv" >"$TMP/b1.log" 2>&1 \
    || { sed -n '1,12p' "$TMP/b1.log"; fail "server did not build"; }
"$AE" build "$SCRIPT_DIR/client.ae" -o "$TMP/cli" >"$TMP/b2.log" 2>&1 \
    || { sed -n '1,12p' "$TMP/b2.log"; fail "pure-TLS client did not build"; }

CERT_PATH="$TMP/cert.pem" KEY_PATH="$TMP/key.pem" "$TMP/srv" >"$TMP/srv.log" 2>&1 &
SRV_PID=$!

i=0
while [ "$i" -lt 100 ]; do
    grep -q READY "$TMP/srv.log" 2>/dev/null && break
    sleep 0.1
    i=$((i + 1))
done
grep -q READY "$TMP/srv.log" 2>/dev/null || {
    sed -n '1,10p' "$TMP/srv.log"
    fail "the TLS server never became READY"
}

# SSL_CERT_FILE is how tls13_client is told where its trust anchors are; with
# a self-signed test cert that file IS the anchor. Without it the handshake
# fails closed with "cannot load system trust store", which is correct
# behaviour and worth stating because it is not obvious.
OUT=$(SSL_CERT_FILE="$TMP/cert.pem" "$TMP/cli" 2>&1) || {
    printf '%s\n' "$OUT" | grep -vE 'warning: unresolved|-->' | sed 's/^/    cli: /'
    sed -n '1,6p' "$TMP/srv.log" | sed 's/^/    srv: /'
    fail "the pure-TLS client did not complete the exchange"
}

case "$OUT" in
    *"PASS: pure-Aether TLS 1.3 handshake"*) ;;
    *) echo "$OUT" | sed 's/^/         /'; fail "no handshake confirmation" ;;
esac
case "$OUT" in
    *"PASS: HTTP/1.1 response received over pure TLS"*) ;;
    *) echo "$OUT" | sed 's/^/         /'; fail "no HTTP response over the TLS stream" ;;
esac

echo "  [PASS] https_pure_tls_vertical: std.http server <- HTTP/1.1 over pure-Aether TLS 1.3"
