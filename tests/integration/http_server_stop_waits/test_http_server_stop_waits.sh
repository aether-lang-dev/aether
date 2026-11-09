#!/bin/sh
# std.http: http_server_stop waits for the server's run, whichever start
# began it, so http_server_free after it is safe.
#
# foreground_actor.ae: http_server_start_raw run by an actor, a keep-alive
# connection held open across the stop, stop and free from main. Stop used
# to wait only for a background server, and the free released a foreground
# one under its pool worker: a segfault on macOS and Linux (found by
# servirtium-vcr). Each round also checks the server's monitor reported
# stopped before stop returned.
#
# from_handler.ae: a stop from a handler (one of the server's own threads)
# only signals, and the free from main then waits; a background server
# stopped that way used to hang. A start of a running server is refused.
#
# A watchdog kills each program after LIMIT seconds so a hang fails rather
# than wedging the sweep.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
NAME=http_server_stop_waits
EXE="${EXE_EXT:-}"
if [ -z "$EXE" ] && [ ! -x "$ROOT/build/ae" ] && [ -x "$ROOT/build/ae.exe" ]; then
    EXE=".exe"
fi
AE="$ROOT/build/ae$EXE"
[ -x "$AE" ] || { echo "  [SKIP] $NAME: build/ae not built"; exit 0; }
LIMIT="${STOP_WAITS_LIMIT:-60}"
. "$ROOT/tests/lib/server_jobs.sh"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT

run_case() {
    prog="$1"
    want="$2"
    if ! "$AE" build "$SCRIPT_DIR/$prog.ae" -o "$TMP/$prog$EXE" > "$TMP/$prog.build.log" 2>&1; then
        echo "  [FAIL] $NAME: $prog.ae did not build"
        tail -8 "$TMP/$prog.build.log" | sed 's/^/        /'
        return 1
    fi
    STOP_WAITS_DIR="$TMP" "$TMP/$prog$EXE" > "$TMP/$prog.log" 2>&1 &
    pid=$!
    waited=0
    while server_alive "$pid"; do
        if [ "$waited" -ge "$LIMIT" ]; then
            kill_server "$pid"
            echo "  [FAIL] $NAME: $prog did not finish in ${LIMIT}s (a stop hung)"
            tail -5 "$TMP/$prog.log" | sed 's/^/        /'
            return 1
        fi
        sleep 1
        waited=$((waited + 1))
    done
    wait "$pid"
    rc=$?
    if [ "$rc" -ne 0 ] || ! grep -q "$want" "$TMP/$prog.log"; then
        echo "  [FAIL] $NAME: $prog exited $rc"
        tail -5 "$TMP/$prog.log" | sed 's/^/        /'
        return 1
    fi
    return 0
}

run_case foreground_actor "FOREGROUND-OK 10" || exit 1
run_case from_handler "FROM-HANDLER-OK" || exit 1
echo "  [PASS] $NAME: a foreground server stopped and freed from another thread, a stop from a handler, a refused second start"
exit 0
