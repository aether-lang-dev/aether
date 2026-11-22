#!/bin/sh
# The pure TLS client with a CSPRNG that fails (#3293): a failed draw must
# fail the handshake, never leave a key share, the client random or the
# session id as the zeros of a buffer the generator did not fill.
#
# rngfail.ae links rng_fail_shim.c (aether.toml), which stands in front of
# the OS CSPRNG and fails every draw after the first N. It connects once
# with every draw let through, counting them (D), then once for each N in
# 0..D-1; listener.ae reports what each connect sent. The verdict: no
# ClientHello with an all-zero random or session id, at least one connect
# refused for want of randomness, and the free one a real ClientHello; the
# same draws for tls13_client.connect_psk, whose first must refuse it.
#
# Where the CSPRNG cannot fail (macOS and the BSDs: arc4random_buf) the
# test reports a skip.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"
NAME=crypto_random_failure
[ -x "$AE" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }

TMP="$(mktemp -d)"
LPID=""
cleanup() {
    [ -n "$LPID" ] && stop_server "$LPID"
    rm -rf "$TMP" || :
    return 0
}
trap cleanup EXIT
fail() { echo "  [FAIL] $NAME: $1"; exit 1; }

cd "$SCRIPT_DIR" || exit 1
"$AE" build listener.ae -o "$TMP/listener" >"$TMP/b1.log" 2>&1 || { sed -n '1,12p' "$TMP/b1.log"; fail "listener.ae did not build"; }
"$AE" build rngfail.ae -o "$TMP/rngfail" >"$TMP/b2.log" 2>&1 || { sed -n '1,12p' "$TMP/b2.log"; fail "rngfail.ae did not build"; }

PORT_OUT="$TMP/port" "$TMP/listener" >"$TMP/listener.out" 2>&1 &
LPID=$!
i=0
while [ "$i" -lt 100 ] && [ ! -s "$TMP/port" ]; do sleep 0.1; i=$((i + 1)); done
[ -s "$TMP/port" ] || { cat "$TMP/listener.out"; fail "the listener never listened"; }

PEER_PORT="$(cat "$TMP/port")" "$TMP/rngfail" >"$TMP/client.out" 2>&1
case "$(cat "$TMP/client.out")" in
    SKIP*) echo "  [SKIP] $NAME: $(cat "$TMP/client.out")"; exit 0 ;;
esac
n=0
while server_alive "$LPID" && [ "$n" -lt 100 ]; do sleep 0.1; n=$((n + 1)); done
stop_server "$LPID"
LPID=""

seen="$(cat "$TMP/listener.out")"
said="$(cat "$TMP/client.out")"
ok=1
case "$seen" in *"random_zero=1"*|*"sid_zero=1"*) ok=0 ;; esac
case "$said" in *"randomness"*) ;; *) ok=0 ;; esac
# connect_psk too: its first draw failing refuses it for want of randomness.
case "$said" in *"PSK N=0 ERR: no randomness"*) ;; *) ok=0 ;; esac
case "$(echo "$seen" | head -1)" in "CH random_zero=0 sid_zero=0") ;; *) ok=0 ;; esac
[ -n "${RNG_VERBOSE:-}" ] && { echo "$said"; echo "$seen"; }
if [ "$ok" = 1 ]; then
    echo "    ok: no ClientHello made from undrawn bytes ($(echo "$said" | grep -c '^N=') connects)"
    echo "  [PASS] $NAME"
    exit 0
fi
echo "    client: $(echo "$said" | tr '\n' '|')"
echo "    listener: $(echo "$seen" | tr '\n' '|')"
fail "a ClientHello was made from bytes the CSPRNG did not fill, or no connect was refused"
