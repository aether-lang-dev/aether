#!/bin/sh
# A server frees the state of the access logger and the metrics collector it
# registers (#2910). Before, http_server_free left both behind: the metrics
# state with its mutex and every route it had recorded, and the access log's
# format and open file. shim.c creates 50 servers with both (metrics twice),
# serves requests that make them record, and frees each; on macOS leaks(1)
# must find nothing.

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] http_server_hook_state: the probe serves over a POSIX socket pair"
        exit 0
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] http_server_hook_state: $AE not built"; exit 0; }

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

if ! AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/probe.ae" -o "$TMPDIR/probe" \
        --extra "$SCRIPT_DIR/shim.c" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] http_server_hook_state: build failed"
    sed 's/^/    /' "$TMPDIR/build.log" | head -20
    exit 1
fi

if ! ACCESS_LOG_PATH="$TMPDIR/access.log" "$TMPDIR/probe" >"$TMPDIR/run.log" 2>&1 \
   || ! grep -q "http_server_hook_state passed" "$TMPDIR/run.log"; then
    echo "  [FAIL] http_server_hook_state: probe failed"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi
[ -s "$TMPDIR/access.log" ] || { echo "  [FAIL] http_server_hook_state: nothing was logged"; exit 1; }

if [ "$(uname -s)" = Darwin ] && command -v leaks >/dev/null 2>&1; then
    ACCESS_LOG_PATH="$TMPDIR/access2.log" MallocStackLogging=1 leaks --atExit -- "$TMPDIR/probe" \
        >"$TMPDIR/leaks.log" 2>&1
    if ! grep -q "0 leaks for 0 total leaked bytes" "$TMPDIR/leaks.log"; then
        echo "  [FAIL] http_server_hook_state: leaks(1) found leaks"
        grep -E "leaks for|ROOT LEAK|MetricsState|AccessLog|metrics_hook|access_log" "$TMPDIR/leaks.log" | head -20
        exit 1
    fi
fi

echo "  [PASS] http_server_hook_state"
