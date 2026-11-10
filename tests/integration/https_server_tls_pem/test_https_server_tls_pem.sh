#!/bin/sh
# A TLS server takes its certificate chain and key as PEM text (#3362), and
# sends the whole chain on both backends (#3370).
#
# A root, an intermediate it signs, and a leaf for localhost the intermediate
# signs; the server's certificate file is the leaf and the intermediate. The
# client trusts the root alone, so it verifies the server only if the server
# sends the intermediate.
#
#   1. On each backend, from the text (server_set_tls_pem) and from the files
#      (server_set_tls): the client fetches. The OpenSSL backend loaded the
#      first certificate of the file alone, so its clients could not reach
#      the root.
#   2. The control: served the leaf alone, the same client is refused, so the
#      cases above pass because the chain is sent.
#   3. What server_set_tls_pem refuses, on each backend, and says why: empty
#      text, a certificate that is not PEM, another certificate's key, an
#      encrypted key (refused at once: the OpenSSL backend asked for a
#      passphrase on the terminal before).
#
# In a build without OpenSSL only the "aether" cases run.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/wait_port.sh"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"
NAME=https_server_tls_pem

[ -x "$AE" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }
command -v openssl >/dev/null 2>&1 || { echo "  [SKIP] $NAME: no openssl to make the certificates"; exit 0; }

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
ssl() { "$@" >"$TMP/ssl.log" 2>&1 || { sed -n '1,10p' "$TMP/ssl.log"; fail "openssl: $*"; }; }

printf 'basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n' >"$TMP/ca.ext"
printf 'basicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nsubjectAltName=DNS:localhost\n' >"$TMP/leaf.ext"
for k in root inter leaf other; do
    ssl openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$TMP/$k.key"
done
ssl env MSYS2_ARG_CONV_EXCL='/CN=' openssl req -x509 -new -key "$TMP/root.key" -out "$TMP/root.pem" \
    -days 2 -subj "/CN=PEM Test Root" -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign"
ssl env MSYS2_ARG_CONV_EXCL='/CN=' openssl req -new -key "$TMP/inter.key" -out "$TMP/inter.csr" -subj "/CN=PEM Test Intermediate"
ssl openssl x509 -req -in "$TMP/inter.csr" -CA "$TMP/root.pem" -CAkey "$TMP/root.key" -CAcreateserial \
    -days 2 -sha256 -extfile "$TMP/ca.ext" -out "$TMP/inter.pem"
ssl env MSYS2_ARG_CONV_EXCL='/CN=' openssl req -new -key "$TMP/leaf.key" -out "$TMP/leaf.csr" -subj "/CN=localhost"
ssl openssl x509 -req -in "$TMP/leaf.csr" -CA "$TMP/inter.pem" -CAkey "$TMP/inter.key" -CAcreateserial \
    -days 2 -sha256 -extfile "$TMP/leaf.ext" -out "$TMP/leafonly.pem"
cat "$TMP/leafonly.pem" "$TMP/inter.pem" >"$TMP/chain.pem"
ssl openssl verify -CAfile "$TMP/root.pem" -untrusted "$TMP/inter.pem" "$TMP/leafonly.pem"
ssl openssl pkey -in "$TMP/leaf.key" -aes256 -passout pass:secret -out "$TMP/enc.key"
printf 'not a certificate\n' >"$TMP/garbage.pem"

for p in server client; do
    "$AE" build "$SCRIPT_DIR/$p.ae" -o "$TMP/$p" >"$TMP/b_$p.log" 2>&1 \
        || { sed -n '1,12p' "$TMP/b_$p.log"; fail "$p did not build"; }
done

# Run a program with a watchdog; its output (warnings dropped) lands in $OUT.
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

run env URL="https://localhost:1/" CAFILE="$TMP/root.pem" BACKEND=openssl "$TMP/client"
BACKENDS="aether"
case "$OUT" in *"has no OpenSSL"*) ;; *) BACKENDS="aether openssl" ;; esac

# Start the server on backend $1, mode $2, certificate file $3, key file $4;
# READY -> S_PORT, else its output in $OUT and S_PORT empty. A server that
# hangs (asking for a passphrase, say) counts as an answer too.
start_server() {
    S_PORT=""
    log="$TMP/srv_$1_$2_$(basename "$3")_$(basename "$4").log"
    BACKEND="$1" MODE="$2" CERT_PATH="$3" KEY_PATH="$4" "$TMP/server" >"$log" 2>&1 </dev/null &
    spid=$!
    PIDS="$PIDS $spid"
    i=0
    while [ "$i" -lt 300 ]; do
        grep -q -E 'READY|FAIL' "$log" 2>/dev/null && break
        sleep 0.1
        i=$((i + 1))
    done
    OUT=$(cat "$log")
    [ "$i" -lt 300 ] || OUT="HUNG: $OUT"
    case "$OUT" in
        *"READY "*) S_PORT=$(sed -n 's/^READY \([0-9]*\).*/\1/p' "$log" | head -1) ;;
    esac
}

# 1. The chain from the text and from the files, on each backend.
for b in $BACKENDS; do
    for m in pem path; do
        start_server "$b" "$m" "$TMP/chain.pem" "$TMP/leaf.key"
        [ -n "$S_PORT" ] || { echo "    got: $OUT"; fail "$b backend, $m: the server did not start"; }
        run env URL="https://localhost:$S_PORT/" CAFILE="$TMP/root.pem" BACKEND=aether "$TMP/client"
        case "$OUT" in
            *"STATUS 200 BODY pem-ok"*) echo "    ok: $b backend, $m: the root alone verifies the chain" ;;
            *) echo "    got: $OUT"; fail "$b backend, $m: not fetched with the root alone" ;;
        esac
    done
done

# 2. The control: the leaf alone cannot be verified with the root alone.
for b in $BACKENDS; do
    start_server "$b" pem "$TMP/leafonly.pem" "$TMP/leaf.key"
    [ -n "$S_PORT" ] || { echo "    got: $OUT"; fail "$b backend: the leaf-only server did not start"; }
    run env URL="https://localhost:$S_PORT/" CAFILE="$TMP/root.pem" BACKEND=aether "$TMP/client"
    case "$OUT" in
        *"STATUS 200"*) fail "$b backend: a server sending the leaf alone was verified: the chain cases prove nothing" ;;
        *"ERR: "*) echo "    ok: $b backend: the leaf alone is refused by the same client" ;;
        *) echo "    got: $OUT"; fail "$b backend: leaf-only: no answer from the client" ;;
    esac
done

# 3. What server_set_tls_pem refuses, and says why.
refused() { # backend cert key reason-pattern case-name
    start_server "$1" pem "$2" "$3"
    [ -z "$S_PORT" ] || fail "$1 backend, $5: the server started"
    case "$OUT" in
        *"FAIL: server_set_tls_pem: "*$4*) echo "    ok: $1 backend, $5 (refused)" ;;
        *) echo "    got: $OUT"; fail "$1 backend, $5: expected a refusal matching '$4'" ;;
    esac
}
for b in $BACKENDS; do
    refused "$b" "$TMP/missing.pem" "$TMP/leaf.key" "cert_pem is empty" "empty certificate text"
    refused "$b" "$TMP/chain.pem" "$TMP/missing.key" "key_pem is empty" "empty key text"
    refused "$b" "$TMP/garbage.pem" "$TMP/leaf.key" "certificate" "a certificate that is not PEM"
    refused "$b" "$TMP/chain.pem" "$TMP/other.key" "match" "another certificate's key"
    refused "$b" "$TMP/chain.pem" "$TMP/enc.key" "ncrypted" "an encrypted key"
done

echo "  [PASS] $NAME"
