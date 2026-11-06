#!/bin/sh
# tls13_client over a socket the caller connected: verification off, and a
# client certificate (mutual TLS).
#
# connect_socket (TLS over a socket the program connected itself, for a
# protocol that starts in the clear, Postgres' SSLRequest and MySQL's
# handshake) always verified the server and never presented a client
# certificate; the verify mode and the certificate existed in connect_full
# but nothing exported reached them over an adopted socket.
#
# Against openssl s_server -tls1_3:
#   1. A self-signed server certificate: connect_socket refuses it;
#      connect_socket_insecure_skip_verify connects and gets an answer.
#   2. `s_server -Verify 1 -CAfile root`, a server certificate from the same
#      root: connect_socket_mtls with a client certificate from the root is
#      answered; connect_socket (no certificate) is not; the mtls entry
#      point refuses a missing key before any handshake. The refusal is
#      named: certificate_required (116), which read as unknown_alert.
#   3. `-Verify 1` with the self-signed server certificate:
#      connect_socket_mtls_insecure_skip_verify presents the certificate
#      and is answered.
#
# Skipped without openssl.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"
NAME=crypto_tls13_socket_verify_mtls

[ -x "$AE" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }
command -v openssl >/dev/null 2>&1 || { echo "  [SKIP] $NAME: no openssl"; exit 0; }
unset SSL_CERT_DIR SSL_CERT_FILE AETHER_PURE_TLS || :

TMP="$(mktemp -d)"
. "$ROOT/tests/lib/test_openssl_conf.sh"
test_openssl_conf "$TMP"
PIDS=""
cleanup() {
    for p in $PIDS; do stop_server "$p"; done
    rm -rf "$TMP" || :
    return 0
}
trap cleanup EXIT
fail() { echo "  [FAIL] $NAME: $1"; exit 1; }
ssl() { "$@" >"$TMP/ssl.log" 2>&1 || { sed -n '1,10p' "$TMP/ssl.log"; fail "openssl: $*"; }; }

printf 'basicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nsubjectAltName=DNS:localhost\n' >"$TMP/srv.ext"
printf 'basicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=clientAuth\n' >"$TMP/cli.ext"
for k in root srv cli self; do
    ssl openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$TMP/$k.key"
done
MSYS2_ARG_CONV_EXCL='/CN=' ssl openssl req -x509 -new -key "$TMP/root.key" -out "$TMP/root.pem" -days 2 \
    -subj "/CN=Socket Test Root" -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign"
for k in srv cli; do
    MSYS2_ARG_CONV_EXCL='/CN=' ssl openssl req -new -key "$TMP/$k.key" -out "$TMP/$k.csr" -subj "/CN=$k"
    ssl openssl x509 -req -in "$TMP/$k.csr" -CA "$TMP/root.pem" -CAkey "$TMP/root.key" -CAcreateserial \
        -days 2 -extfile "$TMP/$k.ext" -out "$TMP/$k.pem"
done
# A self-signed localhost certificate nothing trusts.
MSYS2_ARG_CONV_EXCL='/CN=' ssl openssl req -x509 -new -key "$TMP/self.key" -out "$TMP/self.pem" -days 2 \
    -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost"

"$AE" build "$SCRIPT_DIR/sockclient.ae" -o "$TMP/sockclient" >"$TMP/build.log" 2>&1 \
    || { sed -n '1,12p' "$TMP/build.log"; fail "sockclient.ae did not build"; }

# serve <server cert stem> <mode> [extra s_server args...]: one s_server
# connection; the client's line lands in $got.
serve() {
    stem=$1; mode=$2; shift 2
    SP=""
    for try in 1 2 3 4 5; do
        P=$(( 20000 + ($$ * 13 + try * 1013) % 40000 ))
        openssl s_server -4 -www -accept "$P" -tls1_3 -naccept 1 "$@" \
            -cert "$TMP/$stem.pem" -key "$TMP/$stem.key" >"$TMP/s.log" 2>&1 </dev/null &
        spid=$!
        PIDS="$PIDS $spid"
        w=0
        while [ "$w" -lt 100 ] && server_alive "$spid" && ! grep -q ACCEPT "$TMP/s.log" 2>/dev/null; do sleep 0.1; w=$((w + 1)); done
        if server_alive "$spid" && grep -q ACCEPT "$TMP/s.log" 2>/dev/null; then
            SP=$P
            break
        fi
        kill_server "$spid"
    done
    [ -n "$SP" ] || fail "could not start openssl s_server"
    got=$(MODE="$mode" CAFILE="$TMP/root.pem" CLIENT_CERT="$TMP/cli.pem" CLIENT_KEY="$TMP/cli.key" PORT="$SP" \
          "$TMP/sockclient" 2>&1 | grep -v 'warning' || :)
    if [ "$mode" = mtls_nokey ]; then
        # Nothing connects to TLS here; let s_server go.
        stop_server "$spid"
        return 0
    fi
    n=0
    while server_alive "$spid" && [ "$n" -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
    stop_server "$spid"
}

fails=0
expect() {
    label=$1; pat=$2
    case "$got" in
        $pat) echo "    ok: $label" ;;
        *) echo "    FAIL: $label: client [$got], server [$(grep -i 'error\|alert\|verify' "$TMP/s.log" | tr '\n' '|')]"; fails=$((fails + 1)) ;;
    esac
}

serve self verify
expect "self-signed server, verification on: refused" "REFUSED: *"
serve self skip
expect "self-signed server, insecure_skip_verify: answered" "*GOT HTTP/1.0 200 ok*"
serve srv mtls -Verify 1 -CAfile "$TMP/root.pem"
expect "-Verify 1: connect_socket_mtls presents the certificate and is answered" "*GOT HTTP/1.0 200 ok*"
serve srv verify -Verify 1 -CAfile "$TMP/root.pem"
case "$got" in
    *"GOT HTTP"*) echo "    FAIL: -Verify 1 answered a client with no certificate: [$got]"; fails=$((fails + 1)) ;;
    *"REFUSED: "*certificate_required*|*"RECVERR: "*certificate_required*) echo "    ok: -Verify 1: no client certificate, not answered (${got%%
*})" ;;
    *) echo "    FAIL: -Verify 1, no certificate: client [$got]"; fails=$((fails + 1)) ;;
esac
serve srv mtls_nokey -Verify 1 -CAfile "$TMP/root.pem"
expect "connect_socket_mtls without a key: refused before the handshake" "*REFUSED: connect_socket_mtls: the client certificate or key is missing*"
serve self mtls_skip -Verify 1 -CAfile "$TMP/root.pem"
expect "-Verify 1, self-signed server: mtls_insecure_skip_verify is answered" "*GOT HTTP/1.0 200 ok*"

[ "$fails" = 0 ] || fail "$fails case(s)"
echo "  [PASS] $NAME"
