#!/bin/sh
# An https client gives up on a server that never answers its ClientHello at
# the request's timeout (#3703, the client side), through each TLS backend
# the build has. stalled.ae sets a 500 ms timeout against a listener that
# never accepts; each round must fail within ROUND_LIMIT_MS. The pure-Aether
# client used to wait about 25 minutes there (std.tcp's 30 s timeout in place
# of the request's, and 50 retries of a timed-out read): the run is bounded
# by RUN_LIMIT seconds and a run that outlasts it is a failure.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
NAME=https_client_handshake_timeout
EXE="${EXE_EXT:-}"
if [ -z "$EXE" ] && [ ! -x "$ROOT/build/ae" ] && [ -x "$ROOT/build/ae.exe" ]; then
    EXE=".exe"
fi
AE="$ROOT/build/ae$EXE"
[ -x "$AE" ] || { echo "  [SKIP] $NAME: build/ae not built"; exit 0; }
ROUND_LIMIT_MS=2500
RUN_LIMIT=30
. "$ROOT/tests/lib/server_jobs.sh"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT

if ! AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/stalled.ae" -o "$TMP/stalled$EXE" \
        >"$TMP/build.log" 2>&1; then
    echo "  [FAIL] $NAME: build failed"
    sed 's/^/    /' "$TMP/build.log" | head -20
    exit 1
fi

"$TMP/stalled$EXE" >"$TMP/run.log" 2>&1 &
pid=$!
waited=0
while server_alive "$pid"; do
    if [ "$waited" -ge "$RUN_LIMIT" ]; then
        kill_server "$pid"
        echo "  [FAIL] $NAME: the client was still in its handshake after ${RUN_LIMIT}s"
        sed 's/^/    /' "$TMP/run.log" | head -10
        exit 1
    fi
    sleep 1
    waited=$((waited + 1))
done
wait "$pid"

if ! grep -q "^DONE" "$TMP/run.log"; then
    echo "  [FAIL] $NAME: the program did not finish"
    sed 's/^/    /' "$TMP/run.log" | head -10
    exit 1
fi

ran=0
while read -r tag backend what ms rest; do
    [ "$tag" = ROUND ] || continue
    [ "$what" = ELAPSED ] || continue
    ran=$((ran + 1))
    case "$rest" in
        "ERR none"*)
            echo "  [FAIL] $NAME: [$backend] a response from a server that sends nothing"
            exit 1 ;;
    esac
    if [ "$ms" -ge "$ROUND_LIMIT_MS" ]; then
        echo "  [FAIL] $NAME: [$backend] gave up after ${ms} ms, past ${ROUND_LIMIT_MS} ms for a 500 ms timeout"
        sed 's/^/    /' "$TMP/run.log" | head -10
        exit 1
    fi
done < "$TMP/run.log"

if [ "$ran" -eq 0 ]; then
    echo "  [FAIL] $NAME: no TLS backend ran"
    sed 's/^/    /' "$TMP/run.log" | head -10
    exit 1
fi
echo "  [PASS] $NAME: $ran backend(s) gave up on a silent server at the request's timeout"
