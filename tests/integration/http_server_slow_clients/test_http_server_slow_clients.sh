#!/bin/sh
# The slow and abusive client harness (#3737, #3750): every time limit a
# server puts on a client, driven by a client that sends a byte just inside
# the per-read idle timeout, over and over. That client held a worker for as
# long as it liked through a TLS handshake (on either backend), a request
# head read over TLS or on a server with HTTP/2 on, and a request body
# however it was read. Each now has an absolute budget (the body's extended
# by a minimum rate), answered 408 or closed past it. shim.c has the rounds;
# the old code failed every one but rate and sleep, which must succeed.
#
# On Linux and macOS it does the same against the proxy event loop, a
# gateway proxying to an upstream, whose idle timer moved with every byte:
# a slow head, body and handshake, a body above the minimum rate, and the
# loop's connection bound (server_set_loop_max_connections).
#
# The TLS rounds need openssl(1) for a certificate; the ones that send a
# TLS head or body slowly need the build to link OpenSSL, for a client.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
NAME=http_server_slow_clients
EXE="${EXE_EXT:-}"
if [ -z "$EXE" ] && [ ! -x "$ROOT/build/ae" ] && [ -x "$ROOT/build/ae.exe" ]; then
    EXE=".exe"
fi
AE="$ROOT/build/ae$EXE"
[ -x "$AE" ] || { echo "  [SKIP] $NAME: build/ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT

# The probe's bound is the sweep's own: never timeout(1) here (see
# tests/lib/sweep_timeout.sh).
. "$ROOT/tests/lib/sweep_timeout.sh"
sweep_timeout_bound TIMEOUT "$ROOT" 120

# The TLS client: OpenSSL's when the build links it and the client builds,
# else none (those rounds skip).
TLS_CLIENT="$SCRIPT_DIR/tls_client_none.c"
if "$AE" cflags 2>/dev/null | grep -q -- '-lssl'; then
    TLS_CLIENT="$SCRIPT_DIR/tls_client.c"
fi
build() {
    AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/probe.ae" -o "$TMP/probe$EXE" \
        --extra "$SCRIPT_DIR/shim.c" --extra "$1" >"$TMP/build.log" 2>&1
}
if ! build "$TLS_CLIENT"; then
    if [ "$TLS_CLIENT" = "$SCRIPT_DIR/tls_client.c" ] && build "$SCRIPT_DIR/tls_client_none.c"; then
        echo "  (no OpenSSL client could be built: the slow TLS head and body rounds skip)"
    else
        echo "  [FAIL] $NAME: build failed"
        sed 's/^/    /' "$TMP/build.log" | head -20
        exit 1
    fi
fi

# A path as the native programs here read it: openssl(1) and the probe are
# Windows programs under MSYS2, where cygpath gives the form they open (and
# MSYS2_ARG_CONV_EXCL keeps "/CN=..." from being taken for a path).
native() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf '%s' "$1"; fi
}

cert=""
if command -v openssl >/dev/null 2>&1 &&
   MSYS2_ARG_CONV_EXCL="*" openssl req -x509 -newkey rsa:2048 \
       -keyout "$(native "$TMP/key.pem")" -out "$(native "$TMP/cert.pem")" \
       -days 1 -nodes -subj "/CN=localhost" >"$TMP/openssl.log" 2>&1; then
    cert="$(native "$TMP/cert.pem")"
    key="$(native "$TMP/key.pem")"
fi

SLOW_PROBE_CERT="$cert" SLOW_PROBE_KEY="${cert:+$key}" \
    $TIMEOUT "$TMP/probe$EXE" >"$TMP/run.log" 2>&1
RC=$?
sed 's/^/    /' "$TMP/run.log"
[ "$RC" = "0" ] || { echo "  [FAIL] $NAME: the probe exited $RC"; exit 1; }
grep -q "$NAME passed" "$TMP/run.log" || { echo "  [FAIL] $NAME: a slow client was not cut off in time"; exit 1; }

echo "  [PASS] $NAME: $(grep -c '^round.*passed$' "$TMP/run.log") rounds, every slow client cut off within its budget"
