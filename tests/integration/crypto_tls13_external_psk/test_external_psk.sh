#!/bin/sh
# The pure TLS client's external PSK (tls13_client.connect_psk and
# connect_socket_psk, RFC 8446 2.2 and 4.2.11), against three peers:
#
#   1. pskpeer.ae, a scripted server that computes every secret itself and
#      misbehaves on request, for what no real server does on purpose: a
#      selected_identity other than 0, a psk_ke answer (no key_share), a
#      SHA-384 suite the client did not offer, a ServerHello that does not
#      take the PSK, a Certificate after taking it, a wrong Finished, a
#      TLS 1.2 ServerHello, a server that skips the binder check with
#      another key. It also says which label keyed the binder: "ext" for
#      "ext binder", "res" for "res binder" (7.1).
#   2. openssl s_server -psk -psk_identity -nocert -tls1_3, independent of
#      this tree: the host and adopted-socket paths, a HelloRetryRequest
#      (-groups P-256, the client's first ClientHello has an x25519 share
#      alone), wrong key, wrong identity, SHA-384 only, a certificate
#      server with no PSK, TLS 1.2.
#   3. Python's ssl with SSLContext.set_psk_server_callback (3.13 and
#      later, ssl.HAS_PSK), independent too: host, socket, P-256 retry,
#      wrong key and identity, a certificate server, TLS 1.2 PSK.
#
# And who closes the socket handed to connect_socket_psk (ownclient.ae
# against sink.py): the call, on every failure path, before it returns.
#
# 1 and the socket ownership need nothing beyond loopback and a python3.
# 2 is skipped, saying so, without an openssl whose s_server has -psk and
# -tls1_3; 3 without a python3 that has ssl.HAS_PSK (PSK_PYTHON names one).

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"
NAME=crypto_tls13_external_psk

[ -x "$AE" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }

TMP="$(mktemp -d)"
PIDS=""
cleanup() {
    for p in $PIDS; do stop_server "$p"; done
    rm -rf "$TMP" || :
    return 0
}
trap cleanup EXIT
fail() { echo "  [FAIL] $NAME: $1"; exit 1; }

for p in pskpeer pskclient ownclient; do
    "$AE" build "$SCRIPT_DIR/$p.ae" -o "$TMP/$p" >"$TMP/b_$p.log" 2>&1 \
        || { sed -n '1,12p' "$TMP/b_$p.log"; fail "$p did not build"; }
done

KEY=1111111111111111111111111111111111111111111111111111111111111111
OTHER_KEY=2222222222222222222222222222222222222222222222222222222222222222
fails=0
note() {
    if [ "$1" = ok ]; then
        echo "    ok: $2"
    else
        echo "    FAIL: $2: $3"
        fails=$((fails + 1))
    fi
}

# scripted <name> <PEER_CASE> <client env> <client expects> <peer expects>
# Both are matched as shell patterns against the outputs joined with "|".
scripted() {
    rm -f "$TMP/port" "$TMP/peer.out"
    PEER_CASE="$2" PEER_PORT_OUT="$TMP/port" PSK_IDENTITY_HEX="${PEER_ID:-}" "$TMP/pskpeer" >"$TMP/peer.out" 2>&1 &
    ppid=$!
    PIDS="$PIDS $ppid"
    i=0
    while [ "$i" -lt 100 ] && [ ! -s "$TMP/port" ]; do sleep 0.1; i=$((i + 1)); done
    [ -s "$TMP/port" ] || { cat "$TMP/peer.out"; fail "$1: the peer never listened"; }
    # shellcheck disable=SC2086
    got=$(env $3 PSK_PORT="$(cat "$TMP/port")" "$TMP/pskclient" 2>&1 | tr '\n' '|')
    n=0
    while server_alive "$ppid" && [ "$n" -lt 100 ]; do sleep 0.1; n=$((n + 1)); done
    stop_server "$ppid"
    seen=$(tr '\n' '|' <"$TMP/peer.out")
    ok=1
    # shellcheck disable=SC2254
    case "$got" in $4) ;; *) ok=0 ;; esac
    # shellcheck disable=SC2254
    case "$seen" in $5) ;; *) ok=0 ;; esac
    if [ "$ok" = 1 ]; then note ok "$1"; else note no "$1" "client said [$got], peer saw [$seen]"; fi
}

echo "  scripted peer"
scripted "connect_psk: ext binder, SHA-256 suites alone, data both ways" "" "" \
    "CONNECTED group=29|SENT|RECV hi|" "CH id=ok binder=ext suites=1303,1301 early=0|FINISHED ok|DATA hello|"
scripted "connect_socket_psk" "" "PSK_MODE=socket" \
    "CONNECTED group=29|SENT|RECV hi|" "CH id=ok binder=ext suites=1303,1301 early=0|FINISHED ok|DATA hello|"
scripted "HelloRetryRequest for secp256r1: binder again over the new transcript" "hrr=group:0017" "" \
    "CONNECTED group=23|SENT|RECV hi|" "*CH2 id=ok binder=ext|FINISHED ok|DATA hello|"
PEER_ID=00706c7567000a
scripted "an identity with NUL bytes" "" "PSK_IDENTITY_HEX=00706c7567000a" \
    "CONNECTED group=29|SENT|RECV hi|" "CH id=ok binder=ext*FINISHED ok|DATA hello|"
PEER_ID=$KEY$KEY
scripted "a 64-byte identity, the longest" "" "PSK_IDENTITY_HEX=$KEY$KEY" \
    "CONNECTED group=29|SENT|RECV hi|" "CH id=ok binder=ext*FINISHED ok|DATA hello|"
PEER_ID=
scripted "wrong key: the server's binder check refuses it" "" "PSK_KEY_HEX=$OTHER_KEY" \
    "ERR: server refused the handshake with alert decrypt_error (51)|" "CH id=ok binder=bad*REFUSED|"
scripted "wrong key, server skips the binder: its flight does not open" "ignore_binder=1" "PSK_KEY_HEX=$OTHER_KEY" \
    "ERR: authentication failed|" "CH id=ok binder=bad*UNREADABLE*"
scripted "wrong identity" "" "PSK_IDENTITY_HEX=706c75672d3032" \
    "ERR: server refused the handshake with alert decrypt_error (51)|" "CH id=bad*REFUSED|"
scripted "a server Finished that does not verify" "badfinished=1" "" \
    "ERR: external PSK: server Finished MAC verification failed|" "*ALERT 51|"
scripted "selected_identity 1 (one identity offered)" "psk_id=1" "" \
    "ERR: server hello: selected_identity 1, but the client offered one PSK|" "*ALERT 47|"
scripted "psk_ke: no key_share in the ServerHello" "nokeyshare=1" "" \
    "ERR: server hello: no key_share, but the client offered only psk_dhe_ke|" "*ALERT 109|"
scripted "TLS_AES_256_GCM_SHA384, not offered" "suite=1302" "" \
    "ERR: server hello: cipher suite 4866 was not offered|" "*ALERT 47|"
scripted "TLS_AES_256_GCM_SHA384 in a HelloRetryRequest" "hrr=group:0017;suite=1302" "" \
    "ERR: hello retry request: cipher suite 4866 was not offered|" "*ALERT 47|"
scripted "a ServerHello that does not take the PSK" "nopsk=1" "" \
    "ERR: server did not take the external PSK (no pre_shared_key in its ServerHello)|" "*ALERT 40|"
scripted "a Certificate after taking the PSK" "cert=1" "" \
    "ERR: handshake: a message of type 11 out of its place in the server's flight|" "*ALERT 10|"
scripted "a TLS 1.2 ServerHello" "tls12=1" "" \
    "ERR: server hello: not TLS 1.3*" "*ALERT 70|"

echo "  socket ownership (connect_socket_psk closes the socket on every failure)"
if command -v python3 >/dev/null 2>&1; then
    rm -f "$TMP/sport"
    python3 -I "$SCRIPT_DIR/sink.py" "$TMP/sport" 700 6 >"$TMP/sink.out" 2>&1 &
    skpid=$!
    PIDS="$PIDS $skpid"
    i=0
    while [ "$i" -lt 100 ] && [ ! -s "$TMP/sport" ]; do sleep 0.1; i=$((i + 1)); done
    [ -s "$TMP/sport" ] || { cat "$TMP/sink.out"; fail "the sink never listened"; }
    own=$(SINK_PORT="$(cat "$TMP/sport")" "$TMP/ownclient" 2>&1 | tr '\n' '|')
    n=0
    while server_alive "$skpid" && [ "$n" -lt 100 ]; do sleep 0.1; n=$((n + 1)); done
    stop_server "$skpid"
    sunk=$(tr '\n' '|' <"$TMP/sink.out")
    want="CASE 0 true connect_socket_psk: the socket is null|CASE 1 external PSK: the identity must be 1 to 64 bytes, not 0|CASE 2 external PSK: the identity must be 1 to 64 bytes, not 65|CASE 3 external PSK: the key must be 32 bytes (SHA-256), not 31|CASE 4 external PSK: the key must be 32 bytes (SHA-256), not 33|CASE 5 external PSK: the key must be 32 bytes (SHA-256), not -1|CASE 6 record: *|DONE|"
    # shellcheck disable=SC2254
    case "$own" in $want) ok=1 ;; *) ok=0 ;; esac
    case "$sunk" in *OPEN*) ok=0 ;; esac
    [ "$(printf '%s' "$sunk" | tr '|' '\n' | grep -c ' EOF ')" = 6 ] || ok=0
    if [ "$ok" = 1 ]; then
        note ok "refused arguments and a refused handshake each close the socket before returning"
    else
        note no "socket ownership" "client said [$own], sink saw [$sunk]"
    fi
else
    echo "    skip: socket ownership: no python3 for the sink"
fi

echo "  deadlines and cancellation (tcp.connect_until, tcp.set_deadline, conn_set_deadline)"
"$AE" build "$SCRIPT_DIR/tlsdeadline.ae" -o "$TMP/tlsdeadline" >"$TMP/b_tlsdeadline.log" 2>&1 \
    || { sed -n '1,12p' "$TMP/b_tlsdeadline.log"; fail "tlsdeadline did not build"; }
# stop_case <CASE> <PEER_CASE> <want error> <earliest ms> <latest ms> <after>
# `after` is a pattern for what follows the result: the next conn_recv's
# answer and how soon close_conn returned, or "" for a handshake case.
stop_case() {
    rm -f "$TMP/port" "$TMP/peer.out"
    HOLD_MS=15000 PEER_CASE="$2" PEER_PORT_OUT="$TMP/port" "$TMP/pskpeer" >"$TMP/peer.out" 2>&1 &
    ppid=$!
    PIDS="$PIDS $ppid"
    i=0
    while [ "$i" -lt 100 ] && [ ! -s "$TMP/port" ]; do sleep 0.1; i=$((i + 1)); done
    [ -s "$TMP/port" ] || fail "$1: the peer never listened"
    got=$(CASE="$1" PSK_PORT="$(cat "$TMP/port")" "$TMP/tlsdeadline" 2>&1 | tr '\n' '|')
    stop_server "$ppid"
    ms=$(printf '%s' "$got" | sed -n 's/.*RESULT [^|]* ms=\([0-9]*\).*/\1/p')
    ok=1
    case "$got" in *"RESULT $1 err=[$3] "*) ;; *) ok=0 ;; esac
    # shellcheck disable=SC2254
    case "$got" in *$6*) ;; *) ok=0 ;; esac
    [ -n "$ms" ] && [ "$ms" -ge "$4" ] && [ "$ms" -le "$5" ] || ok=0
    if [ "$ok" = 1 ]; then
        note ok "$1: [$3] after $ms ms (bound $4..$5)"
    else
        note no "$1" "client said [$got]; want [$3] in $4..$5 ms"
    fi
}
AFTER="AFTER recv=?recv: the connection failed earlier?|CLOSED in "
stop_case hs-deadline stall=1 "deadline exceeded" 700 2200 ""
stop_case hs-cancel stall=1 "cancelled" 400 1900 ""
stop_case hs-adopted stall=1 "deadline exceeded" 700 2200 ""
stop_case after-deadline stall_after=1 "deadline exceeded" 1000 2500 "$AFTER"
stop_case after-cancel stall_after=1 "cancelled" 400 1900 "$AFTER"
stop_case trickle trickle=1 "deadline exceeded" 1000 2500 "$AFTER"
# Sent 1 MiB at a time: the stop can come one piece's sealing late.
stop_case write-deadline noread=1 "deadline exceeded" 1000 4000 "$AFTER"
stop_case write-cancel noread=1 "cancelled" 400 3400 "$AFTER"
stop_case ordinary "" "" 0 2000 "RECV hi"
stop_case wrong-key "" "server refused the handshake with alert decrypt_error (51)" 0 2000 ""
dial=$(CASE=dial-deadline STALL_ADDR="${STALL_ADDR:-192.0.2.1}" "$TMP/tlsdeadline" 2>&1 | tr '\n' '|')
case "$dial" in
    *"err=[deadline exceeded]"*)
        ms=$(printf '%s' "$dial" | sed -n 's/.*ms=\([0-9]*\).*/\1/p')
        if [ "$ms" -ge 700 ] && [ "$ms" -le 2200 ]; then note ok "dial-deadline: a stalled dial ends at the deadline ($ms ms)"
        else note no "dial-deadline" "after $ms ms"; fi ;;
    *) echo "    skip: dial-deadline: ${STALL_ADDR:-192.0.2.1} does not stall a connect here [$dial]" ;;
esac

# ---- openssl s_server ----
if command -v openssl >/dev/null 2>&1 && openssl s_server -help 2>&1 | grep -q -- '-psk_identity' \
    && openssl s_server -help 2>&1 | grep -q -- '-tls1_3'; then
    echo "  openssl s_server ($(openssl version | cut -d' ' -f1-2))"
    . "$ROOT/tests/lib/test_openssl_conf.sh"
    test_openssl_conf "$TMP"
    MSYS2_ARG_CONV_EXCL='/CN=' openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -keyout "$TMP/k.pem" -out "$TMP/c.pem" -days 2 -nodes -subj "/CN=localhost" >"$TMP/ssl.log" 2>&1 \
        || { sed -n '1,12p' "$TMP/ssl.log"; fail "openssl could not make the certificate"; }
    # s_case <name> <client env> <client expects> <s_server flags...>
    s_case() {
        sc_name="$1"
        sc_env="$2"
        sc_want="$3"
        shift 3
        SP=""
        for try in 1 2 3 4 5; do
            P=$(( 20000 + ($$ * 7 + try * 1009 + $(printf '%s' "$sc_name" | wc -c)) % 40000 ))
            openssl s_server -4 -www -accept "$P" -naccept 1 "$@" >"$TMP/s.log" 2>&1 </dev/null &
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
        [ -n "$SP" ] || { sed -n '1,8p' "$TMP/s.log"; fail "$sc_name: could not start openssl s_server"; }
        # shellcheck disable=SC2086
        got=$(env $sc_env REQUEST=GET PSK_PORT="$SP" "$TMP/pskclient" 2>&1 | tr '\n' '|')
        n=0
        while server_alive "$spid" && [ "$n" -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
        stop_server "$spid"
        # shellcheck disable=SC2254
        case "$got" in
            $sc_want) note ok "s_server: $sc_name" ;;
            *) note no "s_server: $sc_name" "client said [$got]; server [$(grep -i -E 'error|alert|psk' "$TMP/s.log" | head -3 | tr '\n' '|')]" ;;
        esac
    }
    PSK="-nocert -psk $KEY -psk_identity plug-01"
    # shellcheck disable=SC2086
    s_case "connect_psk" "" "CONNECTED group=29|SENT|RECV HTTP/1.0 200 ok|" -tls1_3 $PSK
    # shellcheck disable=SC2086
    s_case "connect_socket_psk" "PSK_MODE=socket" "CONNECTED group=29|SENT|RECV HTTP/1.0 200 ok|" -tls1_3 $PSK
    # shellcheck disable=SC2086
    s_case "-groups P-256: a HelloRetryRequest" "" "CONNECTED group=23|SENT|RECV HTTP/1.0 200 ok|" -tls1_3 -groups P-256 $PSK
    # shellcheck disable=SC2086
    s_case "-groups P-256, adopted socket" "PSK_MODE=socket" "CONNECTED group=23|SENT|RECV HTTP/1.0 200 ok|" -tls1_3 -groups P-256 $PSK
    # shellcheck disable=SC2086
    s_case "-ciphersuites TLS_CHACHA20_POLY1305_SHA256" "" "CONNECTED group=29|SENT|RECV HTTP/1.0 200 ok|" -tls1_3 -ciphersuites TLS_CHACHA20_POLY1305_SHA256 $PSK
    # shellcheck disable=SC2086
    s_case "wrong key" "PSK_KEY_HEX=$OTHER_KEY" "ERR: server refused the handshake with alert *" -tls1_3 $PSK
    # shellcheck disable=SC2086
    s_case "wrong identity" "PSK_IDENTITY_HEX=706c75672d3032" "ERR: server refused the handshake *" -tls1_3 $PSK
    # shellcheck disable=SC2086
    s_case "TLS_AES_256_GCM_SHA384 alone (the client offers SHA-256 suites only)" "" "ERR: server refused the handshake *" -tls1_3 -ciphersuites TLS_AES_256_GCM_SHA384 $PSK
    s_case "a certificate server with no PSK: no fallback" "" \
        "ERR: server did not take the external PSK (no pre_shared_key in its ServerHello)|" -tls1_3 -cert "$TMP/c.pem" -key "$TMP/k.pem"
    # shellcheck disable=SC2086
    s_case "-tls1_2 with the PSK: refused" "" "ERR: server does not support TLS 1.3 *" -tls1_2 $PSK
else
    echo "    skip: openssl s_server: no openssl, or its s_server has no -psk_identity or -tls1_3"
fi

# ---- Python ssl, 3.13 and later ----
PYPSK=""
for c in "${PSK_PYTHON:-}" python3 python3.13 python3.14 python3.15; do
    [ -n "$c" ] || continue
    if command -v "$c" >/dev/null 2>&1 && "$c" -c 'import ssl, sys; sys.exit(0 if getattr(ssl, "HAS_PSK", False) and sys.version_info >= (3, 13) else 1)' 2>/dev/null; then
        PYPSK="$c"
        break
    fi
done
if [ -n "$PYPSK" ]; then
    echo "  Python ssl ($("$PYPSK" -c 'import ssl, sys; print(sys.version.split()[0], ssl.OPENSSL_VERSION)'))"
    if [ ! -f "$TMP/c.pem" ]; then
        openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -keyout "$TMP/k.pem" -out "$TMP/c.pem" \
            -days 2 -nodes -subj "/CN=localhost" >"$TMP/ssl.log" 2>&1 || fail "openssl could not make the certificate"
    fi
    # py_case <name> <peer case> <client env> <client expects> <peer expects>
    py_case() {
        rm -f "$TMP/pport" "$TMP/py.out"
        "$PYPSK" -I "$SCRIPT_DIR/psk_peer.py" "$TMP/pport" "$2" "$TMP/c.pem" "$TMP/k.pem" >"$TMP/py.out" 2>&1 &
        pypid=$!
        PIDS="$PIDS $pypid"
        i=0
        while [ "$i" -lt 100 ] && [ ! -s "$TMP/pport" ]; do sleep 0.1; i=$((i + 1)); done
        [ -s "$TMP/pport" ] || { cat "$TMP/py.out"; fail "$1: the Python peer never listened"; }
        # shellcheck disable=SC2086
        got=$(env $3 REQUEST=GET PSK_PORT="$(cat "$TMP/pport")" "$TMP/pskclient" 2>&1 | tr '\n' '|')
        n=0
        while server_alive "$pypid" && [ "$n" -lt 100 ]; do sleep 0.1; n=$((n + 1)); done
        stop_server "$pypid"
        # Python on Windows writes text with \r\n: the \r is not the peer's.
        seen=$(tr -d '\r' <"$TMP/py.out" | tr '\n' '|')
        ok=1
        # shellcheck disable=SC2254
        case "$got" in $4) ;; *) ok=0 ;; esac
        # shellcheck disable=SC2254
        case "$seen" in $5) ;; *) ok=0 ;; esac
        if [ "$ok" = 1 ]; then note ok "python: $1"; else note no "python: $1" "client said [$got], peer saw [$seen]"; fi
    }
    py_case "connect_psk" psk "" "CONNECTED group=29|SENT|RECV HTTP/1.1 200 OK|" "GOT GET / HTTP/1.0|"
    py_case "connect_socket_psk" psk "PSK_MODE=socket" "CONNECTED group=29|SENT|RECV HTTP/1.1 200 OK|" "GOT GET / HTTP/1.0|"
    py_case "P-256 alone: a HelloRetryRequest" psk-p256 "" "CONNECTED group=23|SENT|RECV HTTP/1.1 200 OK|" "GOT GET / HTTP/1.0|"
    py_case "P-256 alone, adopted socket" psk-p256 "PSK_MODE=socket" "CONNECTED group=23|SENT|RECV HTTP/1.1 200 OK|" "GOT GET / HTTP/1.0|"
    py_case "wrong key" psk "PSK_KEY_HEX=$OTHER_KEY" "ERR: server refused the handshake *" "SERVER-ERR *"
    py_case "wrong identity" psk "PSK_IDENTITY_HEX=6f74686572" "ERR: server refused the handshake *" "SERVER-ERR *"
    py_case "a certificate server with no PSK: no fallback" cert "" \
        "ERR: server did not take the external PSK (no pre_shared_key in its ServerHello)|" "SERVER-ERR *"
    py_case "TLS 1.2 with the PSK: refused" tls12-psk "" "ERR: server does not support TLS 1.3 *" "SERVER-ERR *"
else
    echo "    skip: Python ssl external PSK: no python3 of 3.13 or later with ssl.HAS_PSK (set PSK_PYTHON to one)"
fi

[ "$fails" = 0 ] || fail "$fails case(s)"
echo "  [PASS] $NAME"
