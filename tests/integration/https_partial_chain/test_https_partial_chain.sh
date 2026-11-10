#!/bin/sh
# A CA file that holds an intermediate CA alone anchors a path only when the
# request trusts partial chains, on both TLS backends (#3311). #3310 made the
# pure backend end a path only at a self-signed root, as OpenSSL does by
# default; client.set_partial_chain is the explicit way back, the same on
# both (OpenSSL's X509_V_FLAG_PARTIAL_CHAIN).
#
# A root CA issues an intermediate CA, which issues the server's leaf for
# localhost; openssl s_server presents the leaf and the intermediate. For
# each backend:
#   1. trusting the intermediate alone is refused, and accepted with
#      set_partial_chain;
#   2. trusting the root works with and without it;
#   3. a trusted certificate that is not a CA still issues nothing: a leaf
#      signed by a basicConstraints cA:FALSE certificate is refused with
#      that certificate trusted and partial chains on;
#   4. the server's leaf itself pinned is refused, and accepted with
#      set_partial_chain.
#
# In a build without OpenSSL only the "aether" client cases run.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/wait_port.sh"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"
NAME=https_partial_chain

[ -x "$AE" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }
command -v openssl >/dev/null 2>&1 || { echo "  [SKIP] $NAME: no openssl to make certificates"; exit 0; }
. "$ROOT/tests/lib/test_openssl_conf.sh"
if ! openssl_has_option s_server -cert_chain; then
    echo "  [SKIP] $NAME: $(openssl version) s_server has no -cert_chain to send an intermediate"
    exit 0
fi

unset SSL_CERT_FILE SSL_CERT_DIR AETHER_PURE_TLS || :

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

# key NAME: a P-256 key in $TMP/NAME.key
key() {
    openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$TMP/$1.key" >"$TMP/ssl.log" 2>&1 \
        || { sed -n '1,10p' "$TMP/ssl.log"; fail "could not make the key $1"; }
}
# sign NAME ISSUER SUBJECT EXTENSIONS: NAME's certificate, issued by ISSUER
sign() {
    MSYS2_ARG_CONV_EXCL='/CN=' \
    openssl req -new -key "$TMP/$1.key" -subj "$3" -out "$TMP/$1.csr" >"$TMP/ssl.log" 2>&1 \
        || { sed -n '1,10p' "$TMP/ssl.log"; fail "could not make the request for $1"; }
    printf '%s\n' "$4" | tr ';' '\n' > "$TMP/$1.ext"
    openssl x509 -req -in "$TMP/$1.csr" -CA "$TMP/$2.pem" -CAkey "$TMP/$2.key" -CAcreateserial \
        -days 2 -sha256 -extfile "$TMP/$1.ext" -out "$TMP/$1.pem" >"$TMP/ssl.log" 2>&1 \
        || { sed -n '1,10p' "$TMP/ssl.log"; fail "could not sign $1"; }
}

key root
MSYS2_ARG_CONV_EXCL='/CN=' \
openssl req -x509 -new -key "$TMP/root.key" -subj "/CN=Partial Chain Test Root" -days 2 -sha256 \
    -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" \
    -out "$TMP/root.pem" >"$TMP/ssl.log" 2>&1 \
    || { sed -n '1,10p' "$TMP/ssl.log"; fail "could not make the root"; }
key inter
sign inter root "/CN=Partial Chain Test Intermediate" \
    "basicConstraints=critical,CA:TRUE;keyUsage=critical,keyCertSign,cRLSign"
key leaf
sign leaf inter "/CN=localhost" \
    "subjectAltName=DNS:localhost;keyUsage=critical,digitalSignature;extendedKeyUsage=serverAuth"
# A certificate that is not a CA, used as if it were one.
key notca
sign notca root "/CN=Partial Chain Test Not A CA" \
    "basicConstraints=critical,CA:FALSE;keyUsage=critical,digitalSignature,keyCertSign"
key leaf2
sign leaf2 notca "/CN=localhost" \
    "subjectAltName=DNS:localhost;keyUsage=critical,digitalSignature;extendedKeyUsage=serverAuth"

"$AE" build "$SCRIPT_DIR/client.ae" -o "$TMP/client" >"$TMP/b_client.log" 2>&1 \
    || { sed -n '1,12p' "$TMP/b_client.log"; fail "client did not build"; }

# Run a client with a watchdog; its output (warnings dropped) lands in $OUT.
run() {
    "$@" >"$TMP/out" 2>&1 &
    cpid=$!
    n=0
    while kill -0 "$cpid" 2>/dev/null; do
        n=$((n + 1))
        if [ "$n" -gt 300 ]; then
            kill_server "$cpid"
            OUT="HUNG (no result after 30 s)"
            return 0
        fi
        sleep 0.1
    done
    wait "$cpid" 2>/dev/null || :
    OUT=$(grep -vE 'warning: unresolved|-->' "$TMP/out" || :)
}
accepted() { # case-name
    case "$OUT" in
        *"STATUS 200"*) echo "    ok: $1" ;;
        *) echo "    got: $OUT"; fail "$1: refused" ;;
    esac
}
refused() { # case-name
    case "$OUT" in
        *"STATUS 200"*) echo "    got: $OUT"; fail "$1: accepted" ;;
        *"ERR"*) echo "    ok: $1 (refused)" ;;
        *) echo "    got: $OUT"; fail "$1: no verdict" ;;
    esac
}

run env URL="https://localhost:1/" CAFILE="$TMP/root.pem" BACKEND=openssl "$TMP/client"
BACKENDS="aether"
case "$OUT" in *"has no OpenSSL"*) ;; *) BACKENDS="aether openssl" ;; esac

# Start openssl s_server with certificate $1, key $2 and chain $3 -> S_PORT.
start_s_server() {
    S_PORT=""
    for try in 1 2 3 4 5; do
        P=$(( 20000 + ($$ * 7 + try * 1009 + $4) % 40000 ))
        openssl s_server -4 -accept "$P" -cert "$TMP/$1.pem" -key "$TMP/$2.key" \
            -cert_chain "$TMP/$3.pem" -www >"$TMP/s_$1.log" 2>&1 &
        spid=$!
        PIDS="$PIDS $spid"
        if wait_port "$P" 127.0.0.1 100 >/dev/null 2>&1 && kill -0 "$spid" 2>/dev/null; then
            S_PORT=$P
            return 0
        fi
        kill_server "$spid"
    done
    sed -n '1,10p' "$TMP/s_$1.log"
    fail "could not start openssl s_server with $1"
}
start_s_server leaf leaf inter 1
GOOD=$S_PORT
start_s_server leaf2 leaf2 notca 2
NOTCA=$S_PORT

for b in $BACKENDS; do
    echo "  backend $b:"
    run env URL="https://localhost:$GOOD/" CAFILE="$TMP/inter.pem" BACKEND="$b" "$TMP/client"
    refused "the intermediate alone, without partial chains"
    run env URL="https://localhost:$GOOD/" CAFILE="$TMP/inter.pem" PARTIAL=1 BACKEND="$b" "$TMP/client"
    accepted "the intermediate alone, with partial chains"
    run env URL="https://localhost:$GOOD/" CAFILE="$TMP/root.pem" BACKEND="$b" "$TMP/client"
    accepted "the root, without partial chains"
    run env URL="https://localhost:$GOOD/" CAFILE="$TMP/root.pem" PARTIAL=1 BACKEND="$b" "$TMP/client"
    accepted "the root, with partial chains"
    run env URL="https://localhost:$NOTCA/" CAFILE="$TMP/notca.pem" PARTIAL=1 BACKEND="$b" "$TMP/client"
    refused "a trusted certificate that is not a CA, with partial chains"
    run env URL="https://localhost:$GOOD/" CAFILE="$TMP/leaf.pem" BACKEND="$b" "$TMP/client"
    refused "the leaf itself pinned, without partial chains"
    run env URL="https://localhost:$GOOD/" CAFILE="$TMP/leaf.pem" PARTIAL=1 BACKEND="$b" "$TMP/client"
    accepted "the leaf itself pinned, with partial chains"
done

echo "  [PASS] $NAME"
