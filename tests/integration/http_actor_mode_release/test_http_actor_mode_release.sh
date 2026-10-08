#!/bin/sh
# An HTTP server in actor dispatch mode releases its per-connection workers
# (#2509).
#
# dispatch_to_worker spawned a worker actor for every data-ready connection
# and never called the release_fn it was given, so each request left an actor
# allocated and registered in its core's table: memory and the tables grew
# with traffic. The server now runs its own step around the one it is given
# and releases the worker when that step has finished the connection.
#
# shim.c serves 300 one-request connections, 40 keep-alive connections of
# five requests and 40 connections that end in an error, then checks that the
# actor count never rose by more than a handful and came back to where it
# started, with every released worker freed. Before the fix it ended about
# 380 actors up.

# Actor dispatch mode is POSIX-only: the accept poller behind it is not
# built on Windows, where the server always uses its thread pool.
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] http_actor_mode_release: actor dispatch mode is POSIX-only"
        exit 0
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] http_actor_mode_release: $AE not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

if ! AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/probe.ae" -o "$TMPDIR/probe" \
        --extra "$SCRIPT_DIR/shim.c" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] http_actor_mode_release: build failed"
    sed 's/^/    /' "$TMPDIR/build.log" | head -20
    exit 1
fi

if ! "$TMPDIR/probe" >"$TMPDIR/run.log" 2>&1; then
    echo "  [FAIL] http_actor_mode_release: probe exited non-zero"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi

if ! grep -q "http_actor_mode_release passed" "$TMPDIR/run.log"; then
    echo "  [FAIL] http_actor_mode_release: workers not released"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi

echo "  [PASS] http_actor_mode_release"
