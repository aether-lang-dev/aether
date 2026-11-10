#!/bin/sh
# Choosing the TLS backend per client request and per server through
# std.http (#1298): client.set_tls_backend(req, "aether" | "openssl") and
# http.server_set_tls_backend(server, ...), each returning an error string
# when the named backend is not built in or not linked.
#
# Every client backend against every server backend, then the cases that
# prove which backend actually ran (each side's TLS 1.2 peer: the pure stack
# speaks only TLS 1.3, OpenSSL negotiates 1.2), that a choice beats
# AETHER_PURE_TLS, and the refusals. In a build without OpenSSL only the
# "aether" cases and the "openssl" refusal run.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/wait_port.sh"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"
NAME=https_tls_backend_select

[ -x "$AE" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }
command -v openssl >/dev/null 2>&1 || { echo "  [SKIP] $NAME: no openssl to make a certificate"; exit 0; }

unset SSL_CERT_FILE SSL_CERT_DIR AETHER_PURE_TLS || :

TMP="$(mktemp -d)"
# Certificates carry the extensions asked for, under any openssl (#2776).
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

# A self-signed ECDSA P-256 certificate for 127.0.0.1 (the pure server signs
# only with P-256). MSYS2_ARG_CONV_EXCL: see https_pure_tls_vertical.
MSYS2_ARG_CONV_EXCL='/CN=' \
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -keyout "$TMP/key.pem" -out "$TMP/cert.pem" \
    -days 2 -nodes -subj "/CN=127.0.0.1" \
    -addext "subjectAltName=IP:127.0.0.1,DNS:localhost" >"$TMP/ssl.log" 2>&1 \
    || { sed -n '1,10p' "$TMP/ssl.log"; fail "could not generate a test certificate"; }

for p in server server_nomod client client_nomod; do
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

start_server() { # name backend -> PORT
    SRV_BACKEND="$2" CERT_PATH="$TMP/cert.pem" KEY_PATH="$TMP/key.pem" "$TMP/server" >"$TMP/$1.log" 2>&1 &
    PIDS="$PIDS $!"
    i=0
    while [ "$i" -lt 100 ]; do
        grep -q -E 'READY|BACKEND-ERR|FAIL' "$TMP/$1.log" 2>/dev/null && break
        sleep 0.1
        i=$((i + 1))
    done
    grep -q READY "$TMP/$1.log" 2>/dev/null || { sed -n '1,10p' "$TMP/$1.log"; fail "server $1 never became READY"; }
    PORT=$(read_ready_port "$TMP/$1.log") || exit 1
}

# Does this build have OpenSSL? Ask the client API.
run env URL="https://127.0.0.1:1/" BACKEND=openssl "$TMP/client"
HAVE_OPENSSL=1
case "$OUT" in
    *"has no OpenSSL"*) HAVE_OPENSSL=0; echo "    ok: \"openssl\" refused in a build without OpenSSL" ;;
esac
BACKENDS="aether"
[ "$HAVE_OPENSSL" = 1 ] && BACKENDS="aether openssl"

# 1. Every client backend against every server backend.
for sb in $BACKENDS; do
    start_server "srv_$sb" "$sb"
    eval "PORT_$sb=\$PORT"
    for cb in $BACKENDS; do
        run env URL="https://127.0.0.1:$PORT/" CAFILE="$TMP/cert.pem" BACKEND="$cb" "$TMP/client"
        expect "client $cb -> server $sb" "STATUS 200 BODY backend-select-ok"
    done
done

if [ "$HAVE_OPENSSL" = 1 ] && openssl s_client -help 2>&1 | grep -q -- '-tls1_2'; then
    # 2. The server backend really is the one chosen: a TLS 1.2-only client
    #    completes a handshake with OpenSSL and not with the pure server
    #    (s_client prints "New, TLSv1.2, Cipher is ..." only on success;
    #    LibreSSL's prints "New, TLSv1/SSLv3, Cipher is ..." on success and
    #    "New, (NONE), Cipher is (NONE)" on failure, whatever the version).
    S12A=$(echo | openssl s_client -tls1_2 -connect "127.0.0.1:$PORT_aether" 2>&1 || :)
    S12O=$(echo | openssl s_client -tls1_2 -connect "127.0.0.1:$PORT_openssl" 2>&1 || :)
    case "$S12O" in
        *"New, TLSv1.2"*|*"New, TLSv1/SSLv3, Cipher is "[A-Za-z0-9]*) echo "    ok: the \"openssl\" server speaks TLS 1.2" ;;
        *) printf '%s\n' "$S12O" | sed -n '1,8p'; fail "the \"openssl\" server refused TLS 1.2" ;;
    esac
    case "$S12A" in
        *"New, TLSv1.2"*|*"New, TLSv1/SSLv3, Cipher is "[A-Za-z0-9]*) fail "the \"aether\" server negotiated TLS 1.2: it is not the pure server" ;;
        *) echo "    ok: the \"aether\" server refuses TLS 1.2 (the pure stack)" ;;
    esac
fi

if [ "$HAVE_OPENSSL" = 1 ] && openssl s_server -help 2>&1 | grep -q -- '-tls1_2'; then
    # 3. The client backend really is the one chosen, against a TLS 1.2-only
    #    server: "aether" says it needs TLS 1.3, "openssl" gets its page, and
    #    the choice wins over AETHER_PURE_TLS=1.
    S12=""
    for try in 1 2 3 4 5; do
        P=$(( 20000 + ($$ * 7 + try * 1009) % 40000 ))
        openssl s_server -4 -accept "$P" -cert "$TMP/cert.pem" -key "$TMP/key.pem" \
            -tls1_2 -www >"$TMP/s12.log" 2>&1 &
        spid=$!
        PIDS="$PIDS $spid"
        if wait_port "$P" 127.0.0.1 100 >/dev/null 2>&1 && kill -0 "$spid" 2>/dev/null; then
            S12=$P
            break
        fi
        kill_server "$spid"
    done
    [ -n "$S12" ] || fail "could not start openssl s_server -tls1_2"
    run env URL="https://127.0.0.1:$S12/" CAFILE="$TMP/cert.pem" BACKEND=aether "$TMP/client"
    expect "client \"aether\" against TLS 1.2 refuses" "does not support TLS 1.3"
    run env AETHER_PURE_TLS=1 URL="https://127.0.0.1:$S12/" CAFILE="$TMP/cert.pem" BACKEND=openssl "$TMP/client"
    expect "client \"openssl\" beats AETHER_PURE_TLS=1" "STATUS 200"
fi

# 4. Refusals, before any network: an unknown name, and "aether" without the
#    import, for the client and for the server.
run env URL="https://127.0.0.1:1/" BACKEND=gnutls "$TMP/client"
expect "client: unknown backend" "unknown TLS backend"
run "$TMP/client_nomod"
expect "client: pure client not linked" "import std.cryptography.tls13_client"
run "$TMP/server_nomod"
expect "server: pure server not linked" "import std.cryptography.tls13_server"
SRV_BACKEND=gnutls CERT_PATH="$TMP/cert.pem" KEY_PATH="$TMP/key.pem" "$TMP/server" >"$TMP/bad.log" 2>&1 || :
OUT=$(cat "$TMP/bad.log")
expect "server: unknown backend" "unknown TLS backend"

echo "  [PASS] $NAME"
