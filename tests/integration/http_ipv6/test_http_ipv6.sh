#!/bin/sh
# std.http over IPv6: the server binds "::1" and "::" (dual-stack), the
# client dials bracketed literals, over plain HTTP and pure-Aether TLS 1.3.
#
#   1. a "::1" listener answers http://[::1]:P/ and sees the peer as ::1
#   2. a "::" listener answers both http://127.0.0.1:P/ and http://[::1]:P/,
#      and reports the IPv4 client as 127.0.0.1, not ::ffff:127.0.0.1
#   3. a "localhost" listener still binds IPv4 only, as before
#   4. the same dual-stack listener over pure TLS 1.3, both ends pure Aether,
#      the certificate naming 127.0.0.1 and ::1 as iPAddress SANs
#   5. TinyWeb on "::": a WebSocket echo, SSE and a long poll through [::1]
#      and 127.0.0.1, over plain HTTP, over TLS (wss://, OpenSSL server)
#      and over pure-Aether TLS 1.3 (SSE and the long poll; see streams.ae
#      for why wss is not run against the pure server)
#   6. a name whose first address hangs (localhost -> ::1, held by
#      blackhole.py, the server on 127.0.0.1): the first connection moves
#      on after a short try, and later ones go straight to the family that
#      worked
#   7. under libaether_sandbox.so (Linux), a tcp grant for 127.0.0.1 admits
#      127.0.0.1 and ::ffff:127.0.0.1 but not ::1, and one for [::1] admits
#      ::1 alone: a grant for one family never opens the other
#
# The "other family refuses" checks in 1 and 3 hold that family's port bound
# but not listening (hold_port.c) while they run: a test beside this one
# could otherwise be handed the same port number on the other family and
# answer (crypto_tls13_peer did, in CI run 43124040748). A port already
# taken there is not one the check can use, so the server is started again
# on a new one.
#
# Skips, and says so, when this host cannot bind ::1.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/wait_port.sh"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] http_ipv6: ae not built"; exit 0; }

TMP="$(mktemp -d)"
SRV_PID=""
HOLD_PID=""
cleanup() {
    stop_server "$HOLD_PID"
    stop_server "$SRV_PID"
    rm -rf "$TMP" || :
    return 0
}
trap cleanup EXIT
fail() { echo "  [FAIL] http_ipv6: $1"; exit 1; }

"$AE" build "$SCRIPT_DIR/server.ae" -o "$TMP/srv" >"$TMP/b1.log" 2>&1 \
    || { sed -n '1,12p' "$TMP/b1.log"; fail "server did not build"; }
"$AE" build "$SCRIPT_DIR/client.ae" -o "$TMP/cli" >"$TMP/b2.log" 2>&1 \
    || { sed -n '1,12p' "$TMP/b2.log"; fail "client did not build"; }
"$AE" build "$SCRIPT_DIR/dial.ae" -o "$TMP/dial" >"$TMP/b3.log" 2>&1 \
    || { sed -n '1,12p' "$TMP/b3.log"; fail "dial did not build"; }
RAW_SOCKET_LIBS=""
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) RAW_SOCKET_LIBS="-lws2_32" ;;
esac
${CC:-cc} -I"$ROOT/tests/lib" "$SCRIPT_DIR/hold_port.c" -o "$TMP/hold" $RAW_SOCKET_LIBS >"$TMP/b5.log" 2>&1 \
    || { sed -n '1,12p' "$TMP/b5.log"; fail "hold_port did not build"; }
AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/streams.ae" --lib "$ROOT/contrib" -o "$TMP/streams" >"$TMP/b4.log" 2>&1 \
    || { sed -n '1,12p' "$TMP/b4.log"; fail "streams did not build"; }

# streams LABEL [env...]: run the TinyWeb streams client/server once.
streams() {
    _label="$1"; shift
    env "$@" "$TMP/streams" >"$TMP/streams.log" 2>&1 \
        && grep -q '^PASS$' "$TMP/streams.log" \
        || { sed 's/^/    streams: /' "$TMP/streams.log"; fail "TinyWeb streams over $_label"; }
}

# start HOST LOG [env...]: start the server on HOST, set PORT, or return 3
# when the host can't bind it.
start() {
    _host="$1"; _log="$2"; shift 2
    env BIND_HOST="$_host" "$@" "$TMP/srv" >"$_log" 2>&1 &
    SRV_PID=$!
    _i=0
    while [ "$_i" -lt 100 ]; do
        grep -q -E "READY|BIND FAILED" "$_log" 2>/dev/null && break
        sleep 0.1
        _i=$((_i + 1))
    done
    if grep -q "BIND FAILED" "$_log"; then
        stop_server "$SRV_PID"; SRV_PID=""
        return 3
    fi
    grep -q READY "$_log" || { sed -n '1,10p' "$_log"; fail "server on $_host never became READY"; }
    PORT=$(read_ready_port "$_log") || exit 1
}

# hold HOST PORT: keep HOST:PORT bound and not listening (hold_port.c) in
# HOLD_PID, or return 3 when something else already has it.
hold() {
    "$TMP/hold" "$1" "$2" >"$TMP/hold.log" 2>&1 </dev/null &
    HOLD_PID=$!
    _i=0
    while [ "$_i" -lt 100 ]; do
        grep -q -E "HELD|BIND FAILED" "$TMP/hold.log" 2>/dev/null && break
        sleep 0.05
        _i=$((_i + 1))
    done
    if grep -q HELD "$TMP/hold.log"; then
        return 0
    fi
    stop_server "$HOLD_PID"; HOLD_PID=""
    return 3
}

# start_held HOST OTHER LOG: start the server on HOST, and hold the same
# port on OTHER, the other family's loopback, starting again on a new port
# while OTHER has it already. Returns 3 when HOST can't be bound.
start_held() {
    _try=0
    while [ "$_try" -lt 10 ]; do
        start "$1" "$3" || return 3
        if hold "$2" "$PORT"; then
            return 0
        fi
        stop_server "$SRV_PID"; SRV_PID=""
        _try=$((_try + 1))
    done
    fail "could not hold $2 at the port of a server on $1 in 10 tries"
}

# get URL EXPECTED [env...]: GET URL and require EXPECTED in the output.
get() {
    _url="$1"; _want="$2"; shift 2
    _out=$(env URL="$_url" "$@" "$TMP/cli" 2>&1) || :
    case "$_out" in
        *"$_want"*) ;;
        *) printf '%s\n' "$_out" | sed 's/^/    cli: /'; fail "$_url: expected '$_want'" ;;
    esac
}

# get_fails URL [env...]: GET URL and require a transport error.
get_fails() {
    _url="$1"; shift
    _out=$(env URL="$_url" "$@" "$TMP/cli" 2>&1) || :
    case "$_out" in
        "ERR: "*) ;;
        *) printf '%s\n' "$_out" | sed 's/^/    cli: /'; fail "$_url: expected a connect failure" ;;
    esac
}

# Does this host have IPv6 loopback? Asked of the OS, not of the code under
# test: a server that fails to bind ::1 on a host that has it is a failure,
# not a skip. "unknown" where neither probe applies (Windows), and then the
# server's own bind decides.
ipv6_loopback() {
    if [ -r /proc/net/if_inet6 ]; then
        grep -q '^00000000000000000000000000000001 ' /proc/net/if_inet6 && echo yes || echo no
    elif command -v ifconfig >/dev/null 2>&1; then
        ifconfig lo0 2>/dev/null | grep -q 'inet6 ::1' && echo yes || echo no
    else
        echo unknown
    fi
}
HAS_V6=$(ipv6_loopback)
if [ "$HAS_V6" = no ]; then
    echo "  [SKIP] http_ipv6: this host has no IPv6 loopback"
    exit 0
fi

# --- 1. IPv6 loopback ---
if ! start_held "::1" "127.0.0.1" "$TMP/s1.log"; then
    [ "$HAS_V6" = yes ] && fail "the server could not bind ::1, which this host has"
    echo "  [SKIP] http_ipv6: this host cannot bind ::1 (no IPv6)"
    exit 0
fi
get "http://[::1]:$PORT/" "STATUS 200 BODY ipv6-ok peer=::1"
get_fails "http://127.0.0.1:$PORT/"
stop_server "$HOLD_PID"; HOLD_PID=""
stop_server "$SRV_PID"; SRV_PID=""

# The "::" wildcard is what dual-stack means, and on Windows each new
# executable listening on it opens a firewall prompt (#2639). There the
# dual-stack parts are left out and TLS runs over ::1 alone. OpenBSD has no
# dual-stack at all (inet6(4): an AF_INET6 socket is IPv6-only, and
# IPV6_V6ONLY cannot be cleared), so "::" there answers [::1] and never
# 127.0.0.1: the same parts are left out, for that reason.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) WILDCARD=no; WILDCARD_WHY="Windows (wildcard listener)" ;;
    OpenBSD) WILDCARD=no; WILDCARD_WHY="OpenBSD (no dual-stack sockets)" ;;
    *) WILDCARD=yes ;;
esac

# --- 2. dual-stack "::" ---
if [ "$WILDCARD" = yes ]; then
    start "::" "$TMP/s2.log" || fail "could bind ::1 but not ::"
    get "http://[::1]:$PORT/" "STATUS 200 BODY ipv6-ok peer=::1"
    get "http://127.0.0.1:$PORT/" "STATUS 200 BODY ipv6-ok peer=127.0.0.1"
    stop_server "$SRV_PID"; SRV_PID=""
    DUAL_NOTE="dual-stack ::"
else
    DUAL_NOTE="dual-stack skipped on $WILDCARD_WHY"
fi

# --- 3. "localhost" still means 127.0.0.1 ---
start_held "localhost" "::1" "$TMP/s3.log" || fail "could not bind localhost"
get "http://127.0.0.1:$PORT/" "STATUS 200 BODY ipv6-ok peer=127.0.0.1"
get_fails "http://[::1]:$PORT/"
stop_server "$HOLD_PID"; HOLD_PID=""
stop_server "$SRV_PID"; SRV_PID=""

# --- 4. dual-stack over pure TLS 1.3 ---
if command -v openssl >/dev/null 2>&1; then
    . "$ROOT/tests/lib/test_openssl_conf.sh"
    test_openssl_conf "$TMP"
    # MSYS2_ARG_CONV_EXCL: see https_pure_tls_vertical; keeps the subject
    # from being rewritten as a Windows path.
    MSYS2_ARG_CONV_EXCL='/CN=' \
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -keyout "$TMP/key.pem" -out "$TMP/cert.pem" -days 2 -nodes \
        -subj "/CN=localhost" \
        -addext "subjectAltName=IP:127.0.0.1,IP:::1,DNS:localhost" >"$TMP/ssl.log" 2>&1 \
        || { sed -n '1,10p' "$TMP/ssl.log"; fail "could not generate a test certificate"; }
    TLS_HOST="::"
    [ "$WILDCARD" = yes ] || TLS_HOST="::1"
    start "$TLS_HOST" "$TMP/s4.log" AETHER_PURE_TLS=1 CERT_PATH="$TMP/cert.pem" KEY_PATH="$TMP/key.pem" \
        || fail "could not bind $TLS_HOST for TLS"
    get "https://[::1]:$PORT/" "STATUS 200 BODY ipv6-ok peer=::1" AETHER_PURE_TLS=1 CAFILE="$TMP/cert.pem"
    if [ "$WILDCARD" = yes ]; then
        get "https://127.0.0.1:$PORT/" "STATUS 200 BODY ipv6-ok peer=127.0.0.1" AETHER_PURE_TLS=1 CAFILE="$TMP/cert.pem"
    fi
    stop_server "$SRV_PID"; SRV_PID=""
    TLS_NOTE="pure TLS 1.3 over IPv6"
else
    TLS_NOTE="TLS part skipped: no openssl to make a cert"
fi

# --- 5. TinyWeb WebSocket, SSE and long poll ---
if [ "$WILDCARD" = no ]; then
    STREAMS_NOTE="TinyWeb streams skipped on $WILDCARD_WHY"
else
    streams "plain HTTP"
    STREAMS_NOTE="ws/SSE/long poll over HTTP"
    if [ -f "$TMP/cert.pem" ]; then
        streams "TLS (OpenSSL server)" CERT_PATH="$TMP/cert.pem" KEY_PATH="$TMP/key.pem" \
            CAFILE="$TMP/cert.pem" SSL_CERT_FILE="$TMP/cert.pem"
        streams "pure TLS 1.3" SKIP_WS=1 AETHER_PURE_TLS=1 CERT_PATH="$TMP/cert.pem" \
            KEY_PATH="$TMP/key.pem" CAFILE="$TMP/cert.pem"
        STREAMS_NOTE="ws/wss/SSE/long poll over HTTP and TLS"
    fi
fi

# --- 6. a slow first address costs one short try, once ---
PY="$(sh "$ROOT/tests/scripts/find_python.sh" 2>/dev/null)" || PY=""
SLOW_NOTE="slow-first-address part skipped"
# Linux only: a full accept queue makes Linux drop the SYN, so the connect
# hangs; macOS completes the handshake anyway and the connection then just
# goes unanswered, which is a different case (a silent server, which no
# dial should fall back from).
if [ "$(uname -s)" = Linux ] && [ -n "$PY" ] && \
   [ "$($PY -I -c 'import socket; print(socket.getaddrinfo("localhost", 80, 0, socket.SOCK_STREAM)[0][0] == socket.AF_INET6)' 2>/dev/null)" = True ]; then
    start "127.0.0.1" "$TMP/s6.log" || fail "could not bind 127.0.0.1"
    $PY -I "$SCRIPT_DIR/blackhole.py" "$PORT" "$TMP/bh.ready" >"$TMP/bh.log" 2>&1 &
    BH_PID=$!
    _i=0
    while [ ! -f "$TMP/bh.ready" ] && [ "$_i" -lt 100 ]; do sleep 0.1; _i=$((_i + 1)); done
    if [ -f "$TMP/bh.ready" ]; then
        _out=$(env URL="http://localhost:$PORT/" REPEAT=8 "$TMP/cli" 2>&1) || :
        stop_server "$BH_PID"
        case "$_out" in
            *"STATUS 200 BODY ipv6-ok peer=127.0.0.1"*) ;;
            *) printf '%s\n' "$_out" | sed 's/^/    cli: /'; fail "localhost with a hanging ::1 did not reach 127.0.0.1" ;;
        esac
        _ms=$(printf '%s\n' "$_out" | sed -n 's/^ELAPSED_MS //p')
        # One short try (250 ms) on the first connection, none after it.
        [ -n "$_ms" ] && [ "$_ms" -lt 1500 ] \
            || fail "8 connections to localhost took ${_ms:-?} ms with ::1 hanging (want < 1500: one short try, then the family that worked)"
        SLOW_NOTE="hanging ::1 cost ${_ms} ms over 8 connections"
    else
        stop_server "$BH_PID"
        SLOW_NOTE="slow-first-address part skipped: could not hold ::1:$PORT"
    fi
    stop_server "$SRV_PID"; SRV_PID=""
elif [ "$(uname -s)" != Linux ]; then
    SLOW_NOTE="slow-first-address part skipped: hanging ::1 is simulated on Linux only"
elif [ -z "$PY" ]; then
    SLOW_NOTE="slow-first-address part skipped: no Python"
else
    SLOW_NOTE="slow-first-address part skipped: localhost does not resolve to ::1 first here"
fi

# --- 7. tcp grants don't cross families ---
PRELOAD="$ROOT/build/libaether_sandbox.so"
if [ "$(uname -s)" = Linux ] && [ -f "$PRELOAD" ]; then
    # dial GRANT TARGET EXPECTED: the listener is on ::1 for an IPv6
    # target, 127.0.0.1 otherwise.
    dial() {
        printf 'tcp_listen:*\nenv:TARGET\nenv:LISTEN\ntcp:%s\n' "$1" >"$TMP/grants"
        _listen=v4
        [ "$2" = "::1" ] && _listen=v6
        _got=$(LD_PRELOAD="$PRELOAD" AETHER_SANDBOX_LOG=none AETHER_SANDBOX_GRANTS="$TMP/grants" \
               TARGET="$2" LISTEN="$_listen" "$TMP/dial" 2>&1) || :
        [ "$_got" = "$3" ] || fail "grant tcp:$1, connect to $2: expected $3, got '$_got'"
    }
    dial "127.0.0.1" "127.0.0.1" allowed
    dial "127.0.0.1" "::ffff:127.0.0.1" allowed
    dial "127.0.0.1" "::1" denied
    dial "[::1]" "::1" allowed
    dial "[::1]" "127.0.0.1" denied
    dial "[::1]" "::ffff:127.0.0.1" denied
    SANDBOX_NOTE="grants kept to their family"
else
    SANDBOX_NOTE="grant part skipped: no LD_PRELOAD sandbox here"
fi

echo "  [PASS] http_ipv6: ::1, $DUAL_NOTE, localhost-stays-IPv4, $TLS_NOTE, $STREAMS_NOTE, $SLOW_NOTE, $SANDBOX_NOTE"
