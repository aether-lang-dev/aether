#!/bin/sh
# The pure TLS client checks its own CertificateVerify signature against
# its certificate's key before sending it (#3376), and its nonce is hedged.
#
# A client with a certificate (tls13_client.connect_mtls, mtls.ae) against
# `openssl s_server -Verify 1`, which requires a client certificate and
# checks its signature. One root signs the server's certificate and the
# client's; each side trusts the root.
#
#   1. The client's certificate with its own key: the server takes the
#      signature and answers the request, twice (a hedged nonce is fresh
#      each time; both signatures verify).
#   2. The certificate with another P-256 key, standing in for a signature
#      gone wrong: the client refuses to send it and says so, and the
#      server gets internal_error, not a signature it rejects. Before the
#      check the client sent it, the server refused it with decrypt_error,
#      and the client learnt that only from its first read.
#
# Skipped without openssl.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"
NAME=crypto_tls13_mtls_signature

[ -x "$AE" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }
command -v openssl >/dev/null 2>&1 || { echo "  [SKIP] $NAME: no openssl"; exit 0; }
unset SSL_CERT_DIR AETHER_PURE_TLS || :

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
for k in root srv cli other; do
    ssl openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$TMP/$k.key"
done
ssl env MSYS2_ARG_CONV_EXCL='/CN=' openssl req -x509 -new -key "$TMP/root.key" -out "$TMP/root.pem" -days 2 \
    -subj "/CN=mTLS Test Root" -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign"
for k in srv cli; do
    ssl env MSYS2_ARG_CONV_EXCL='/CN=' openssl req -new -key "$TMP/$k.key" -out "$TMP/$k.csr" -subj "/CN=$k"
    ssl openssl x509 -req -in "$TMP/$k.csr" -CA "$TMP/root.pem" -CAkey "$TMP/root.key" -CAcreateserial \
        -days 2 -sha256 -extfile "$TMP/$k.ext" -out "$TMP/$k.pem"
done

"$AE" build "$SCRIPT_DIR/mtls.ae" -o "$TMP/mtls" >"$TMP/b_mtls.log" 2>&1 \
    || { sed -n '1,12p' "$TMP/b_mtls.log"; fail "mtls.ae did not build"; }

# One s_server connection; the client's line lands in $got, the server's
# log in $TMP/s.log.
fails=0
serve() {
    SP=""
    for try in 1 2 3 4 5; do
        P=$(( 20000 + ($$ * 7 + try * 1009) % 40000 ))
        openssl s_server -4 -www -accept "$P" -tls1_3 -naccept 1 -Verify 1 -CAfile "$TMP/root.pem" \
            -cert "$TMP/srv.pem" -key "$TMP/srv.key" >"$TMP/s.log" 2>&1 </dev/null &
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
    got=$(SSL_CERT_FILE="$TMP/root.pem" CLIENT_CERT="$TMP/cli.pem" CLIENT_KEY="$1" PORT="$SP" "$TMP/mtls" 2>&1 \
          | grep -v 'warning' || :)
    n=0
    while server_alive "$spid" && [ "$n" -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
    stop_server "$spid"
}

for round in 1 2; do
    serve "$TMP/cli.key"
    case "$got" in
        *"GOT HTTP/1.0 200 ok"*) echo "    ok: its own key, round $round: the server took the signature and answered" ;;
        *) echo "    FAIL: its own key, round $round: client [$got], server [$(grep -i -E 'error|alert|verify' "$TMP/s.log" | tr '\n' '|')]"; fails=$((fails + 1)) ;;
    esac
done

serve "$TMP/other.key"
case "$got" in
    *"REFUSED: "*"not sent"*)
        if grep -qi -E "internal error|alert number 80" "$TMP/s.log"; then
            echo "    ok: another key: the client did not send the signature, and the server got internal_error"
        else
            echo "    FAIL: another key: the server's log has no internal_error: [$(grep -i -E 'error|alert' "$TMP/s.log" | tr '\n' '|')]"
            fails=$((fails + 1))
        fi
        ;;
    *) echo "    FAIL: another key: client [$got], server [$(grep -i -E 'error|alert|verify' "$TMP/s.log" | tr '\n' '|')]"; fails=$((fails + 1)) ;;
esac

[ "$fails" = 0 ] || fail "$fails case(s)"
echo "  [PASS] $NAME"
