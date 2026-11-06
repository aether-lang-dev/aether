#!/bin/sh
# The pure-Aether TLS server signs with every key kind a certificate can
# carry (#3326), and refuses a certificate and key it cannot serve when the
# server is set up, as the OpenSSL backend does (#3328).
#
#   1. a P-256 key (SEC1, after an EC PARAMETERS block), P-384 and P-521
#      (PKCS#8), Ed25519, and RSA-2048 (PKCS#8 and PKCS#1): the server starts,
#      and the "aether" and "openssl" clients fetch from it (each verifies the
#      CertificateVerify the key's scheme signs);
#   2. server_set_tls refuses, with the reason: an RSA-1024 key, another
#      certificate's key of the same type, a key of another type, an
#      encrypted key, a missing key file;
#   3. a client that offers no scheme the key signs with (openssl s_client
#      with only ECDSA-P256 against the RSA server) gets handshake_failure.
#   4. the chain is exactly the certificate text's certificates: a key
#      between certificates, or a combined key and certificate file, never
#      goes on the wire, and a CERTIFICATE block that does not parse refuses
#      the text (#3378);
#   5. a chain over 16 KiB goes out in several records and verifies, the
#      server serves the next connection, and a 40000-byte response is
#      whole (#3380);
#   6. a request body of 16384 bytes or more reaches the handler whole
#      (#3381);
#   7. a pipelined request in the record that ends a body is answered on a
#      keep-alive connection, not parked unseen (#3384);
#   8. a KeyUpdate a keep-alive client sends behind its request is handled
#      and is not a request: with every worker's connection holding one,
#      the next connection is answered at once, as they are parked (#3405).
#
# In a build without OpenSSL only the "aether" client cases run.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/wait_port.sh"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"
NAME=https_pure_server_keys

[ -x "$AE" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }
command -v openssl >/dev/null 2>&1 || { echo "  [SKIP] $NAME: no openssl to make keys"; exit 0; }

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

# A self-signed certificate for localhost over the key $1.key -> $1.pem.
cert() {
    ssl env MSYS2_ARG_CONV_EXCL='/CN=' openssl req -x509 -new -key "$TMP/$1.key" -out "$TMP/$1.pem" \
        -days 2 -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost"
}
ssl openssl ecparam -name prime256v1 -genkey -out "$TMP/p256.key"
grep -q "BEGIN EC PARAMETERS" "$TMP/p256.key" || fail "the P-256 fixture has no EC PARAMETERS block"
ssl openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-384 -out "$TMP/p384.key"
ssl openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-521 -out "$TMP/p521.key"
ssl openssl genpkey -algorithm ed25519 -out "$TMP/ed25519.key"
ssl openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$TMP/rsa.key"
# PKCS#1: OpenSSL 3 writes it with -traditional; LibreSSL (the macOS
# openssl) and OpenSSL 1.1 know no such flag and write it by default.
if ! openssl rsa -in "$TMP/rsa.key" -traditional -out "$TMP/rsa1.key" >"$TMP/ssl.log" 2>&1; then
    ssl openssl rsa -in "$TMP/rsa.key" -out "$TMP/rsa1.key"
fi
grep -q "BEGIN RSA PRIVATE KEY" "$TMP/rsa1.key" || fail "the PKCS#1 fixture is not PKCS#1"
for k in p256 p384 p521 ed25519 rsa; do cert "$k"; done
cp "$TMP/rsa.pem" "$TMP/rsa1.pem"
# The refusals.
ssl openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:1024 -out "$TMP/weak.key"
cert weak
ssl openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$TMP/other.key"
ssl openssl pkey -in "$TMP/p256.key" -aes256 -passout pass:secret -out "$TMP/enc.key"

for p in server client pipe keyupdate_park; do
    "$AE" build "$SCRIPT_DIR/$p.ae" -o "$TMP/$p" >"$TMP/b_$p.log" 2>&1 \
        || { sed -n '1,12p' "$TMP/b_$p.log"; fail "$p did not build"; }
done

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

run env URL="https://localhost:1/" CAFILE="$TMP/p256.pem" BACKEND=openssl "$TMP/client"
BACKENDS="aether"
case "$OUT" in *"has no OpenSSL"*) ;; *) BACKENDS="aether openssl" ;; esac

# Start the pure server with certificate $1 and key $2; READY -> S_PORT,
# else the server's output in $OUT and S_PORT empty.
#
# One server at a time: each is used until the next starts, which stops it
# first (#3459). Left running, every server of the test was still alive at
# its end, a dozen of them, each with its scheduler's threads, and the RSA
# server of section 3, which loads the slowest key, did not print READY
# within the 30 s on a two-core Linux runner; it also shared its log file
# with section 1's RSA server, still writing to it. Each start has a log of
# its own, and a server that never says READY or FAIL is reported with
# whether it still runs.
SRV_N=0
start_server() {
    [ -n "${spid:-}" ] && stop_server "$spid"
    S_PORT=""
    SRV_N=$((SRV_N + 1))
    slog="$TMP/srv_${SRV_N}_$1_$2.log"
    CERT_PATH="$TMP/$1.pem" KEY_PATH="$TMP/$2.key" "$TMP/server" >"$slog" 2>&1 &
    spid=$!
    PIDS="$PIDS $spid"
    i=0
    while [ "$i" -lt 300 ]; do
        grep -q 'READY\|FAIL' "$slog" 2>/dev/null && break
        sleep 0.1
        i=$((i + 1))
    done
    OUT=$(cat "$slog")
    case "$OUT" in
        *"READY "*) S_PORT=$(sed -n 's/^READY \([0-9]*\).*/\1/p' "$slog" | head -1) ;;
        "") if server_alive "$spid"; then OUT="(no output after 30 s; the server still runs)"
            else OUT="(no output; the server exited)"; fi ;;
    esac
}

# 1. Every key kind serves both clients.
for k in p256 p384 p521 ed25519 rsa rsa1; do
    start_server "$k" "$k"
    [ -n "$S_PORT" ] || { echo "    got: $OUT"; fail "the $k server did not start"; }
    for b in $BACKENDS; do
        run env URL="https://localhost:$S_PORT/" CAFILE="$TMP/$k.pem" BACKEND="$b" "$TMP/client"
        case "$OUT" in
            *"STATUS 200 BODY keys-ok"*) echo "    ok: $k key, client \"$b\"" ;;
            *) echo "    got: $OUT"; fail "$k key, client \"$b\": not fetched" ;;
        esac
    done
done

# 2. What server_set_tls refuses, and says why.
refused_at_setup() { # cert key reason-pattern case-name
    start_server "$1" "$2"
    [ -z "$S_PORT" ] || fail "$4: the server started"
    case "$OUT" in
        *"FAIL: server_set_tls: "*"$3"*) echo "    ok: $4 (refused)" ;;
        *) echo "    got: $OUT"; fail "$4: expected a refusal containing '$3'" ;;
    esac
}
refused_at_setup weak weak "2048 bits" "an RSA-1024 key"
refused_at_setup p256 other "does not match" "another certificate's P-256 key"
refused_at_setup p256 p384 "key type" "a P-384 key for a P-256 certificate"
refused_at_setup p256 enc "encrypted" "an encrypted key"
refused_at_setup p256 missing "cannot read the TLS private key" "a missing key file"

# 3. No scheme in common: the RSA server and a client that offers ECDSA-P256 only.
start_server rsa rsa
[ -n "$S_PORT" ] || { echo "    got: $OUT"; fail "the RSA server did not start"; }
openssl s_client -connect "127.0.0.1:$S_PORT" -servername localhost -tls1_3 \
    -sigalgs ecdsa_secp256r1_sha256 </dev/null >"$TMP/sclient.log" 2>&1 || :
grep -qi "handshake failure\|alert number 40" "$TMP/sclient.log" \
    || { sed -n '1,15p' "$TMP/sclient.log"; fail "no handshake_failure for a client without the key's scheme"; }
echo "    ok: a client without the key's scheme gets handshake_failure"

# 4. The chain is exactly the certificates in the certificate text (#3378):
#    a key between two certificates, and a combined key-and-certificate file
#    given as both, put only certificates on the wire; a CERTIFICATE block
#    that does not parse refuses the text.
cat "$TMP/p256.pem" "$TMP/p256.key" "$TMP/p384.pem" > "$TMP/mixed.pem"
cp "$TMP/p256.key" "$TMP/mixed.key"
cat "$TMP/p256.key" "$TMP/p256.pem" > "$TMP/combo.pem"
cp "$TMP/combo.pem" "$TMP/combo.key"
{ cat "$TMP/p256.pem"; printf -- '-----BEGIN CERTIFICATE-----\nMAMCAQA=\n-----END CERTIFICATE-----\n'; } > "$TMP/badcert.pem"
cp "$TMP/p256.key" "$TMP/badcert.key"
sent_chain() { # name count: the server for $1 sends $2 certificates, each one
    start_server "$1" "$1"
    [ -n "$S_PORT" ] || { echo "    got: $OUT"; fail "$1: the server did not start"; }
    openssl s_client -connect "127.0.0.1:$S_PORT" -servername localhost -tls1_3 -showcerts \
        </dev/null >"$TMP/chain_$1.log" 2>&1 || :
    got=$(grep -c -- '-----BEGIN CERTIFICATE-----' "$TMP/chain_$1.log" || :)
    grep -q '^Certificate chain' "$TMP/chain_$1.log" && [ "$got" = "$2" ] \
        || { sed -n '1,20p' "$TMP/chain_$1.log"; fail "$1: expected exactly $2 certificates on the wire, got $got"; }
    run env URL="https://localhost:$S_PORT/" CAFILE="$TMP/p256.pem" BACKEND=aether "$TMP/client"
    case "$OUT" in
        *"STATUS 200 BODY keys-ok"*) ;;
        *) echo "    got: $OUT"; fail "$1: the pure client did not fetch" ;;
    esac
    echo "    ok: $1 sends exactly its $2 certificate(s)"
}
sent_chain mixed 2
sent_chain combo 1
refused_at_setup badcert badcert "certificate 2 of the chain does not parse" "a CERTIFICATE block that does not parse"


# 5. Records: a chain over 16 KiB (the leaf, then 70 copies of another
#    certificate) goes out in several records and verifies, the server
#    lives on to serve the next connection, and a 40000-byte response
#    crosses two record boundaries (#3380).
start_server p256 p256
[ -n "$S_PORT" ] || { echo "    got: $OUT"; fail "the P-256 server did not start"; }
for b in $BACKENDS; do
    run env URL="https://localhost:$S_PORT/big" CAFILE="$TMP/p256.pem" BACKEND="$b" LEN_ONLY=1 "$TMP/client"
    case "$OUT" in
        *"STATUS 200 LEN 40000"*) echo "    ok: a 40000-byte response, client \"$b\"" ;;
        *) echo "    got: $OUT"; fail "a 40000-byte response, client \"$b\": not whole" ;;
    esac
done
{ cat "$TMP/p256.pem"; i=0; while [ "$i" -lt 70 ]; do cat "$TMP/p384.pem"; i=$((i + 1)); done; } > "$TMP/bigchain.pem"
cp "$TMP/p256.key" "$TMP/bigchain.key"
[ "$(wc -c < "$TMP/bigchain.pem")" -gt 30000 ] || fail "the big chain is not over 16 KiB of DER"
start_server bigchain bigchain
[ -n "$S_PORT" ] || { echo "    got: $OUT"; fail "the big-chain server did not start"; }
for b in $BACKENDS; do
    for pass in 1 2; do
        run env URL="https://localhost:$S_PORT/" CAFILE="$TMP/p256.pem" BACKEND="$b" "$TMP/client"
        case "$OUT" in
            *"STATUS 200 BODY keys-ok"*) ;;
            *) echo "    got: $OUT"; fail "a chain over 16 KiB, client \"$b\", connection $pass: not fetched" ;;
        esac
    done
    echo "    ok: a chain over 16 KiB, two connections, client \"$b\""
done


# 6. Reads (#3381): a request body of 16384 bytes or more comes in records
#    of 16384 bytes, more than the server's first read takes, so it must
#    keep what one read could not take for the next.
start_server p256 p256
[ -n "$S_PORT" ] || { echo "    got: $OUT"; fail "the P-256 server did not start"; }
for b in $BACKENDS; do
    for blen in 16384 40000; do
        run env URL="https://localhost:$S_PORT/len" CAFILE="$TMP/p256.pem" BACKEND="$b" BODY_LEN="$blen" "$TMP/client"
        case "$OUT" in
            *"STATUS 200 BODY len-$blen"*) ;;
            *) echo "    got: $OUT"; fail "a $blen-byte request body, client \"$b\": not whole" ;;
        esac
    done
    echo "    ok: request bodies of 16384 and 40000 bytes, client \"$b\""
done


# 7. Pipelining (#3384): a GET in the record that ends a POST body, on a
#    keep-alive connection with the park lot running. The server reads the
#    body, keeps the GET in the pure server's tail (#3381), answers the
#    POST, and must see that tail before it parks the connection, as it
#    sees OpenSSL's buffered plaintext with SSL_pending.
start_server p256 p256
[ -n "$S_PORT" ] || { echo "    got: $OUT"; fail "the P-256 server did not start"; }
run env PORT="$S_PORT" CAFILE="$TMP/p256.pem" "$TMP/pipe"
case "$OUT" in
    *"GOT len-10 keys-ok"*) echo "    ok: a pipelined request in the record that ends a body" ;;
    *) echo "    got: $OUT"; fail "a pipelined request in the record that ends a body: not answered" ;;
esac


# 8. A KeyUpdate on a keep-alive connection (#3405), on each server backend.
#    The server runs with 8 workers (the pool's floor, AETHER_HTTP_WORKERS);
#    8 keep-alive connections each send a GET and a KeyUpdate and stay open,
#    the KeyUpdate either right behind the GET (it reaches the server with
#    the request) or 80 ms after the answer (it wakes the parked
#    connection). The KeyUpdate is not data, so each connection must stay
#    parked: a worker left reading it blocks until the 30 s idle timeout,
#    and with all 8 so held the next connection waits for that.
for sb in $BACKENDS; do
    for after in 0 80; do
        export SERVER_BACKEND="$sb" AETHER_HTTP_WORKERS=8
        start_server p256 p256
        unset SERVER_BACKEND AETHER_HTTP_WORKERS
        [ -n "$S_PORT" ] || { echo "    got: $OUT"; fail "the 8-worker P-256 server, backend \"$sb\", did not start"; }
        run env PORT="$S_PORT" CAFILE="$TMP/p256.pem" HELD=8 KU_AFTER_MS="$after" "$TMP/keyupdate_park"
        next_ms=$(printf '%s\n' "$OUT" | sed -n 's/^NEXT \([0-9]*\).*/\1/p' | head -1)
        how="right behind the request"
        [ "$after" = 0 ] || how="${after} ms after the answer"
        case "$OUT" in
            *"HELD 8"*"NEXT "*) ;;
            *) echo "    got: $OUT"; fail "server backend \"$sb\", a KeyUpdate $how: no result" ;;
        esac
        [ "${next_ms:-99999}" -lt 5000 ] || { echo "    got: $OUT"; fail "server backend \"$sb\", a KeyUpdate $how: the next request waited ${next_ms} ms: the workers were held"; }
        echo "    ok: server backend \"$sb\", a KeyUpdate $how on 8 connections: the next request was answered in ${next_ms} ms"
    done
done
echo "  [PASS] $NAME"
