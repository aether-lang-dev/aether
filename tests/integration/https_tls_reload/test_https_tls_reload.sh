#!/bin/sh
# A running TLS server re-loads its certificate while it accepts (#3371).
#
# server_set_tls on a running server swapped the OpenSSL context, and with
# #3362 the pure server's parsed credentials, without a lock: a connection
# accepted between reading the pointer and taking its own reference used
# freed memory. Two certificates for localhost, both signed by one root; two
# actors at once each set the server's TLS 100 times, in turn, while 100
# fetches run, on each backend. Every fetch must succeed and nothing may
# crash (the sanitizer legs run this too). The two at once are the second
# race: each call replaced the server's stored copies of the certificate and
# key outside any lock, so two together freed one old copy twice.
#
# Then the same through a proxy gateway on the OpenSSL backend, whose
# connections the event loop takes on Linux and macOS: it takes the server's
# context for each handshake, and must hold it while a re-load replaces it.
#
# In a build without OpenSSL only the "aether" backend runs.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=https_tls_reload

[ -x "$AE" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }
command -v openssl >/dev/null 2>&1 || { echo "  [SKIP] $NAME: no openssl to make the certificates"; exit 0; }

unset SSL_CERT_FILE SSL_CERT_DIR AETHER_PURE_TLS || :

TMP="$(mktemp -d)"
. "$ROOT/tests/lib/test_openssl_conf.sh"
test_openssl_conf "$TMP"
trap 'rm -rf "$TMP" || :' EXIT
fail() { echo "  [FAIL] $NAME: $1"; exit 1; }
ssl() { "$@" >"$TMP/ssl.log" 2>&1 || { sed -n '1,10p' "$TMP/ssl.log"; fail "openssl: $*"; }; }

printf 'basicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nsubjectAltName=DNS:localhost\n' >"$TMP/leaf.ext"
for k in root a b; do
    ssl openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$TMP/$k.key"
done
ssl env MSYS2_ARG_CONV_EXCL='/CN=' openssl req -x509 -new -key "$TMP/root.key" -out "$TMP/root.pem" \
    -days 2 -subj "/CN=Reload Test Root" -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign"
for k in a b; do
    ssl env MSYS2_ARG_CONV_EXCL='/CN=' openssl req -new -key "$TMP/$k.key" -out "$TMP/$k.csr" -subj "/CN=localhost"
    ssl openssl x509 -req -in "$TMP/$k.csr" -CA "$TMP/root.pem" -CAkey "$TMP/root.key" -CAcreateserial \
        -days 2 -extfile "$TMP/leaf.ext" -out "$TMP/$k.pem"
done

"$AE" build "$SCRIPT_DIR/reload.ae" -o "$TMP/reload" >"$TMP/build.log" 2>&1 \
    || { sed -n '1,12p' "$TMP/build.log"; fail "reload.ae did not build"; }

BACKENDS="aether"
if BACKEND=openssl ROOT_CA="$TMP/root.pem" CERT_A=/nonexistent KEY_A=/nonexistent \
        "$TMP/reload" 2>&1 | grep -q "has no OpenSSL"; then :; else BACKENDS="aether openssl"; fi

# Run the program on backend $1, GATEWAY=$2; what it prints lands in $out,
# and how it ended, should it die without a word.
reload() {
    rc=0
    out=$(BACKEND="$1" GATEWAY="$2" ROOT_CA="$TMP/root.pem" CERT_A="$TMP/a.pem" KEY_A="$TMP/a.key" \
          CERT_B="$TMP/b.pem" KEY_B="$TMP/b.key" "$TMP/reload" 2>&1) || rc=$?
    [ "$rc" -eq 0 ] || out="$out (exit status $rc)"
}

for b in $BACKENDS; do
    reload "$b" 0
    case "$out" in
        *"OK 100 of 100 after 200 loads"*) echo "    ok: $b backend: 100 fetches through 200 re-loads" ;;
        *) echo "$out" | tail -5 | sed 's/^/        /'; fail "$b backend: $out" ;;
    esac
done

# The event loop carries the handshakes of a proxy gateway's connections on
# the OpenSSL backend, and takes its context from the server while re-loads
# replace it. Windows has no event loop: there the gateway's connections are
# the worker path's, which the cases above cover.
case "$BACKENDS" in
    *openssl*)
        reload openssl 1
        case "$out" in
            *"OK 100 of 100 after 200 loads, "*" on the loop"*) ;;
            *) echo "$out" | tail -5 | sed 's/^/        /'; fail "openssl gateway: $out" ;;
        esac
        taken=$(printf '%s\n' "$out" | sed -n 's/.*loads, \([0-9]*\) on the loop.*/\1/p' | tail -1)
        case "$(uname -s)" in
            MINGW*|MSYS*|CYGWIN*|Windows_NT) ;;
            *) [ "${taken:-0}" -gt 0 ] || fail "openssl gateway: the event loop took no connection, so it was not tested" ;;
        esac
        echo "    ok: openssl gateway: 100 fetches through 200 re-loads, $taken on the event loop"
        ;;
esac

echo "  [PASS] $NAME"
