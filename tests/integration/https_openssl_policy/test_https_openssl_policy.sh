#!/bin/sh
# The OpenSSL TLS backend accepts the peers the pure-Aether backend accepts
# (#3312, #3313): security level 2 at least on every client and server
# context, so an RSA key under 2048 bits is refused on both backends
# whatever level the distribution configures, and the hostname is matched
# against SANs only. OPENSSL_CONF points at a configuration whose
# system default is level 1 (which accepts RSA-1024, as OpenSSL 3.0 does), so
# the test means the same on every runner.
#
#   1. a server (openssl s_server) with an RSA-1024 certificate: refused by
#      the "openssl" client and by the "aether" client; the same with
#      RSA-2048 is fetched by both;
#   2. std.http's server with the OpenSSL backend refuses to load an
#      RSA-1024 certificate, and loads the RSA-2048 one.
#   3. the subject CN names nothing (#3313): a certificate naming localhost
#      in its CN alone is refused by both clients, as a SAN is fetched.
#
# In a build without OpenSSL only the "aether" client cases run.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/wait_port.sh"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"
NAME=https_openssl_policy

[ -x "$AE" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }
command -v openssl >/dev/null 2>&1 || { echo "  [SKIP] $NAME: no openssl to make certificates"; exit 0; }
. "$ROOT/tests/lib/test_openssl_conf.sh"
if openssl_is_libressl; then
    # The test sets the peer's policy through OpenSSL's ssl_conf module.
    echo "  [SKIP] $NAME: $(openssl version) has no ssl_conf configuration module"
    exit 0
fi

unset SSL_CERT_FILE SSL_CERT_DIR AETHER_PURE_TLS || :

TMP="$(mktemp -d)"
. "$ROOT/tests/lib/test_openssl_conf.sh"
test_openssl_conf "$TMP"
# The same configuration, with the system default security level set to 1
# (OpenSSL 3.0 builds that read no distribution config run at 1): the level
# the backend must raise to 2 itself.
cat > "$TMP/openssl.cnf" <<'EOF'
openssl_conf = openssl_init
[ openssl_init ]
ssl_conf = ssl_sect
[ ssl_sect ]
system_default = system_default_sect
[ system_default_sect ]
CipherString = DEFAULT:@SECLEVEL=1
[ req ]
distinguished_name = req_dn
[ req_dn ]
EOF
PIDS=""
cleanup() {
    for p in $PIDS; do stop_server "$p"; done
    rm -rf "$TMP" || :
    return 0
}
trap cleanup EXIT
fail() { echo "  [FAIL] $NAME: $1"; exit 1; }

# Self-signed RSA certificates for 127.0.0.1. MSYS2_ARG_CONV_EXCL: see
# https_pure_tls_vertical.
for bits in 1024 2048; do
    MSYS2_ARG_CONV_EXCL='/CN=' \
    openssl req -x509 -newkey "rsa:$bits" -keyout "$TMP/rsa$bits.key" -out "$TMP/rsa$bits.pem" \
        -days 2 -nodes -subj "/CN=127.0.0.1" -sha256 \
        -addext "subjectAltName=IP:127.0.0.1" >"$TMP/ssl.log" 2>&1 \
        || { sed -n '1,10p' "$TMP/ssl.log"; fail "could not make an RSA-$bits certificate"; }
done

for p in server client; do
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

expect() { # case-name pattern
    case "$OUT" in
        *"$2"*) echo "    ok: $1" ;;
        *) echo "    got: $OUT"; fail "$1: expected output containing '$2'" ;;
    esac
}

refused() { # case-name
    case "$OUT" in
        *"STATUS 200"*) echo "    got: $OUT"; fail "$1: accepted" ;;
        *"ERR"*) echo "    ok: $1 (refused)" ;;
        *) echo "    got: $OUT"; fail "$1: no verdict" ;;
    esac
}

run env URL="https://127.0.0.1:1/" BACKEND=openssl "$TMP/client"
HAVE_OPENSSL=1
case "$OUT" in *"has no OpenSSL"*) HAVE_OPENSSL=0 ;; esac
BACKENDS="aether"
[ "$HAVE_OPENSSL" = 1 ] && BACKENDS="aether openssl"

# Start openssl s_server with certificate $1 on a free port -> S_PORT.
start_s_server() {
    S_PORT=""
    for try in 1 2 3 4 5; do
        P=$(( 20000 + ($$ * 7 + try * 1009 + $2) % 40000 ))
        openssl s_server -4 -accept "$P" -cert "$TMP/$1.pem" -key "$TMP/$1.key" -www \
            >"$TMP/s_$1.log" 2>&1 &
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

# 1. The client side.
start_s_server rsa1024 1
WEAK=$S_PORT
start_s_server rsa2048 2
STRONG=$S_PORT
for b in $BACKENDS; do
    run env URL="https://127.0.0.1:$WEAK/" CAFILE="$TMP/rsa1024.pem" BACKEND="$b" "$TMP/client"
    refused "client \"$b\" -> an RSA-1024 server"
    run env URL="https://127.0.0.1:$STRONG/" CAFILE="$TMP/rsa2048.pem" BACKEND="$b" "$TMP/client"
    expect "client \"$b\" -> an RSA-2048 server" "STATUS 200"
done

# 2. The server side, on the OpenSSL backend.
if [ "$HAVE_OPENSSL" = 1 ]; then
    SRV_BACKEND=openssl CERT_PATH="$TMP/rsa1024.pem" KEY_PATH="$TMP/rsa1024.key" "$TMP/server" >"$TMP/srv_weak.log" 2>&1 &
    wpid=$!
    PIDS="$PIDS $wpid"
    i=0
    while [ "$i" -lt 100 ]; do
        grep -q -E 'READY|FAIL' "$TMP/srv_weak.log" 2>/dev/null && break
        sleep 0.1
        i=$((i + 1))
    done
    OUT=$(cat "$TMP/srv_weak.log")
    expect "server (openssl) refuses an RSA-1024 certificate" "an RSA key needs 2048 bits or more"
    SRV_BACKEND=openssl CERT_PATH="$TMP/rsa2048.pem" KEY_PATH="$TMP/rsa2048.key" "$TMP/server" >"$TMP/srv_strong.log" 2>&1 &
    PIDS="$PIDS $!"
    i=0
    while [ "$i" -lt 100 ]; do
        grep -q -E 'READY|FAIL' "$TMP/srv_strong.log" 2>/dev/null && break
        sleep 0.1
        i=$((i + 1))
    done
    OUT=$(cat "$TMP/srv_strong.log")
    expect "server (openssl) loads an RSA-2048 certificate" "READY"
fi


# 3. The subject CN names nothing (#3313): a certificate for "localhost" in
#    its CN alone is refused by both clients; the same name as a SAN is
#    fetched by both.
for c in cnonly san; do
    ext=""
    [ "$c" = san ] && ext="subjectAltName=DNS:localhost"
    MSYS2_ARG_CONV_EXCL='/CN=' \
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -keyout "$TMP/$c.key" -out "$TMP/$c.pem" \
        -days 2 -nodes -subj "/CN=localhost" ${ext:+-addext "$ext"} >"$TMP/ssl.log" 2>&1 \
        || { sed -n '1,10p' "$TMP/ssl.log"; fail "could not make the $c certificate"; }
done
start_s_server cnonly 3
CNONLY=$S_PORT
start_s_server san 4
SAN=$S_PORT
for b in $BACKENDS; do
    run env URL="https://localhost:$CNONLY/" CAFILE="$TMP/cnonly.pem" BACKEND="$b" "$TMP/client"
    refused "client \"$b\" -> a certificate naming localhost in its CN only"
    run env URL="https://localhost:$SAN/" CAFILE="$TMP/san.pem" BACKEND="$b" "$TMP/client"
    expect "client \"$b\" -> a certificate naming localhost as a SAN" "STATUS 200"
done

echo "  [PASS] $NAME"
