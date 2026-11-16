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
#
# Then a stop that comes while a worker is in a 400 ms request: the stop
# must return only once that worker has finished with the server. It
# returned as soon as the accept loop was over, and the program's free of
# the server raced the worker still serving (TSan, in
# http_server_multi_accept).
#
# Then a stop from an actor on core 0 with a connection's worker queued on
# core 0 (#3688): only core 0 can step that worker, and the run waited for
# it while the stop held core 0, so the process hung. The run now waits only
# for the workers in a step; the queued one holds the server until it is
# stepped, closes its connection unserved, and gives the server back.

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
    echo "  [FAIL] http_actor_mode_release: workers not released, or a waiting connection left open at stop"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi

if ! grep -q "http_actor_stop_waits passed" "$TMPDIR/run.log"; then
    echo "  [FAIL] http_actor_mode_release: the stop returned before the worker serving a request had finished"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi

if ! grep -q "http_actor_stop_on_core passed" "$TMPDIR/run.log"; then
    echo "  [FAIL] http_actor_mode_release: a stop from an actor on a queued worker's core hung, or the worker did not close its connection"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi

# Again on a one-core scheduler, where every worker is queued behind the
# actor that stops (#3688): the scheduler takes no more threads than the CPUs
# the process may use (#3106). Where taskset is missing (macOS) the run above,
# with the workers placed on the stopping actor's core, is the case.
if command -v taskset >/dev/null 2>&1 && taskset -c 0 true 2>/dev/null; then
    if ! taskset -c 0 "$TMPDIR/probe" >"$TMPDIR/run1.log" 2>&1 ||
       ! grep -q "http_actor_stop_on_core passed" "$TMPDIR/run1.log"; then
        echo "  [FAIL] http_actor_mode_release: on one core, a stop from an actor with a worker queued hung"
        sed 's/^/    /' "$TMPDIR/run1.log" | head -30
        exit 1
    fi
fi

echo "  [PASS] http_actor_mode_release"
