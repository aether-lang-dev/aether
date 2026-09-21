#!/bin/sh
# Regression for #1092: std.tcp exposes a readiness primitive (poll/poll2)
# and a recv-timeout no longer masquerades as a connection close. Together
# these let a poll-driven client service a server-speaks-first stream that
# a half-duplex relay would stall on.

# Runs on Windows too: std.tcp's poll is a Winsock path of its own
# (WSAPoll and a recv timeout that must not read as a close), which no
# POSIX lane exercises. The skip that stood here ("covered by POSIX
# matrix") hid exactly that half.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
AETHER_CACHE_DIR="$TMPDIR/cache"
export AETHER_CACHE_DIR
SRV_PID=""
cleanup() {
    if [ -n "$SRV_PID" ]; then
        # `ae run` compiles then forks the built binary into a child that
        # holds the listen socket, so killing the wrapper alone orphans it
        # (and leaks the fixed port for the next run). Kill the whole
        # process group where setsid gave us one (Linux); on macOS/BSD
        # setsid is usually absent, so also pkill any process whose argv
        # names this test's server.ae as a portable belt-and-braces reap.
        kill -- "-$SRV_PID" 2>/dev/null || kill "$SRV_PID" 2>/dev/null || true
        wait "$SRV_PID" 2>/dev/null || true
        if command -v pkill >/dev/null 2>&1; then
            pkill -f "$SCRIPT_DIR/server.ae" 2>/dev/null || true
        fi
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

# Start the server as its own process-group leader so cleanup can reap the
# forked child too. setsid puts it in a fresh session/pgid == its pid.
if command -v setsid >/dev/null 2>&1; then
    setsid sh -c 'AETHER_HOME="$1" exec "$2" run "$3"' _ "$ROOT" "$AE" "$SCRIPT_DIR/server.ae" >"$TMPDIR/srv.log" 2>&1 &
else
    AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/server.ae" >"$TMPDIR/srv.log" 2>&1 &
fi
SRV_PID=$!

# No READY-grep or port probe: the server's stdout is block-buffered to
# this pipe while it blocks in accept(), and a probe connect would steal
# the single accept slot. Instead the CLIENT self-synchronizes — it
# retries connect() until the listen socket is up (see client.ae). We
# only guard against the server dying before the client can connect.
sleep 0.5
if ! kill -0 "$SRV_PID" 2>/dev/null; then
    echo "  [FAIL] server died during startup:"
    head -30 "$TMPDIR/srv.log"
    exit 1
fi

AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/client.ae" >"$TMPDIR/client.out" 2>&1 || {
    echo "  [FAIL] client exited non-zero:"
    cat "$TMPDIR/client.out"
    echo "--- server log ---"
    head -30 "$TMPDIR/srv.log"
    exit 1
}

cat "$TMPDIR/client.out"
