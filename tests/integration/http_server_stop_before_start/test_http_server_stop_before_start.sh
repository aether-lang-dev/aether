#!/bin/sh
# std.http: a stop that arrives before the background server thread starts
# must not be lost, and http_server_stop must never hang.
#
# http_server_start_background_raw returned as soon as it spawned the thread,
# and the thread's first act in http_server_start_raw was `is_running = 1`
# (and `stopping = 0`). A stop issued in between was overwritten: the thread
# then ran its accept loop forever, and since 0.799 (#2672)
# http_server_stop joins that thread, so open -> start -> stop with no
# request in between hung (found by servirtium-vcr, whose "close is
# idempotent" check hung every run). cycles.ae does that 50 times.
#
# A watchdog kills the program after LIMIT seconds so a hang fails instead of
# wedging the sweep. The limit also bounds the stop itself: on macOS and the
# BSDs shutting the listening socket does not wake a poll() on it, so each
# stop used to wait out the accept loop's one-second poll (50 s here); the
# server now wakes the loop through a pipe of its own.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
NAME=http_server_stop_before_start
EXE="${EXE_EXT:-}"
if [ -z "$EXE" ] && [ ! -x "$ROOT/build/ae" ] && [ -x "$ROOT/build/ae.exe" ]; then
    EXE=".exe"
fi
AE="$ROOT/build/ae$EXE"
[ -x "$AE" ] || { echo "  [SKIP] $NAME: build/ae not built"; exit 0; }
LIMIT="${STOP_CYCLES_LIMIT:-40}"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT

if ! "$AE" build "$SCRIPT_DIR/cycles.ae" -o "$TMP/cycles$EXE" > "$TMP/build.log" 2>&1; then
    echo "  [FAIL] $NAME: cycles.ae did not build"
    tail -8 "$TMP/build.log" | sed 's/^/        /'
    exit 1
fi

"$TMP/cycles$EXE" > "$TMP/out.log" 2>&1 &
pid=$!
waited=0
while kill -0 "$pid" 2>/dev/null; do
    if [ "$waited" -ge "$LIMIT" ]; then
        kill -9 "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
        echo "  [FAIL] $NAME: 50 start/stop cycles did not finish in ${LIMIT}s (a stop was lost, or each stop waited out the poll)"
        tail -5 "$TMP/out.log" | sed 's/^/        /'
        exit 1
    fi
    sleep 1
    waited=$((waited + 1))
done
wait "$pid"
rc=$?
if [ "$rc" -ne 0 ] || ! grep -q "CYCLES-OK 50" "$TMP/out.log"; then
    echo "  [FAIL] $NAME: cycles.ae exited $rc"
    tail -5 "$TMP/out.log" | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] $NAME: 50 open/start/stop cycles with no request, in ${waited}s"
exit 0
