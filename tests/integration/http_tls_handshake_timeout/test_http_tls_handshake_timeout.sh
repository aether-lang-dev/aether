#!/bin/sh
# The TLS handshake is bounded by the server's receive timeout (#3703).
#
# http_server_drain_connection shook hands before conn_serve put the receive
# timeout on the socket: a client that sent the first bytes of a ClientHello
# and stopped held the worker, a pool thread or an actor-mode core, for as
# long as it kept the connection open (a slowloris on the TLS port). shim.c
# sets a 300 ms keep-alive idle timeout, sends three bytes and stalls, and
# times the server's close: plain (the control: a stalled request head) and
# over TLS through each backend the build has, on the thread pool and in
# actor dispatch mode (POSIX). The old code's TLS rounds stay open until the
# client gives up at 5 s.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
NAME=http_tls_handshake_timeout
EXE="${EXE_EXT:-}"
if [ -z "$EXE" ] && [ ! -x "$ROOT/build/ae" ] && [ -x "$ROOT/build/ae.exe" ]; then
    EXE=".exe"
fi
AE="$ROOT/build/ae$EXE"
[ -x "$AE" ] || { echo "  [SKIP] $NAME: build/ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT

if ! AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/probe.ae" -o "$TMP/probe$EXE" \
        --extra "$SCRIPT_DIR/shim.c" >"$TMP/build.log" 2>&1; then
    echo "  [FAIL] $NAME: build failed"
    sed 's/^/    /' "$TMP/build.log" | head -20
    exit 1
fi

# A path as the native programs here read it: openssl(1) and the probe are
# Windows programs under MSYS2, where cygpath gives the form they open (and
# MSYS2_ARG_CONV_EXCL keeps "/CN=..." from being taken for a path).
native() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf '%s' "$1"; fi
}

# A self-signed certificate for the TLS rounds; without openssl(1) only the
# plain rounds run.
cert=""
if command -v openssl >/dev/null 2>&1 &&
   MSYS2_ARG_CONV_EXCL="*" openssl req -x509 -newkey rsa:2048 -keyout "$(native "$TMP/key.pem")" -out "$(native "$TMP/cert.pem")" \
       -days 1 -nodes -subj "/CN=localhost" >"$TMP/openssl.log" 2>&1; then
    cert="$(native "$TMP/cert.pem")"
    key="$(native "$TMP/key.pem")"
fi

if ! TLS_PROBE_CERT="$cert" TLS_PROBE_KEY="${cert:+$key}" \
        "$TMP/probe$EXE" >"$TMP/run.log" 2>&1; then
    echo "  [FAIL] $NAME: probe exited non-zero"
    sed 's/^/    /' "$TMP/run.log" | head -30
    exit 1
fi

if ! grep -q "http_tls_handshake_timeout passed" "$TMP/run.log"; then
    echo "  [FAIL] $NAME: a stalled client was not closed within a few receive timeouts"
    sed 's/^/    /' "$TMP/run.log" | head -30
    exit 1
fi

# With a certificate, at least one TLS backend must have run.
if [ -n "$cert" ] && grep -q "tls_runs=0" "$TMP/run.log"; then
    echo "  [FAIL] $NAME: no TLS backend ran"
    sed 's/^/    /' "$TMP/run.log" | head -30
    exit 1
fi

echo "  [PASS] $NAME: $(grep -c '^round.*passed$' "$TMP/run.log") rounds closed a stalled client within a few receive timeouts"
