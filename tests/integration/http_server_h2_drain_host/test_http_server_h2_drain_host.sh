#!/bin/sh
# Regression (#3738): a host that serves its own connections
# (http_server_drain_connection) never starts the server, and the HTTP/2
# loop sent GOAWAY once the server was not running, so every HTTP/2
# connection such a host served got a GOAWAY before its first request. The
# loop now keys on the stop (`stopping`). shim.c is the host and its
# client; skips when the build has no libnghttp2.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
NAME=http_server_h2_drain_host
EXE="${EXE_EXT:-}"
if [ -z "$EXE" ] && [ ! -x "$ROOT/build/ae" ] && [ -x "$ROOT/build/ae.exe" ]; then
    EXE=".exe"
fi
AE="$ROOT/build/ae$EXE"
[ -x "$AE" ] || { echo "  [SKIP] $NAME: build/ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT

. "$ROOT/tests/lib/sweep_timeout.sh"
sweep_timeout_bound TIMEOUT "$ROOT" 60

if ! AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/probe.ae" -o "$TMP/probe$EXE" \
        --extra "$SCRIPT_DIR/shim.c" >"$TMP/build.log" 2>&1; then
    echo "  [FAIL] $NAME: build failed"
    sed 's/^/    /' "$TMP/build.log" | head -20
    exit 1
fi
$TIMEOUT "$TMP/probe$EXE" >"$TMP/run.log" 2>&1
RC=$?
sed 's/^/    /' "$TMP/run.log"
if grep -q '^NO_H2' "$TMP/run.log"; then
    echo "  [SKIP] $NAME: built without libnghttp2"
    exit 0
fi
[ "$RC" = "0" ] || { echo "  [FAIL] $NAME: the probe exited $RC"; exit 1; }
grep -q "$NAME passed" "$TMP/run.log" || { echo "  [FAIL] $NAME: the first request was not answered before a GOAWAY"; exit 1; }
echo "  [PASS] $NAME: a host's HTTP/2 connection serves its requests"
