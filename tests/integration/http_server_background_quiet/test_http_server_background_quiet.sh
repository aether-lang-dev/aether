#!/bin/sh
# A backgrounded std.http.server (http_server_start_background_raw) must
# run quietly — no interactive "Server running…/Press Ctrl+C to stop"
# banner — and still serve + stop cleanly. Regression for
# std-http-server-background-sigurg-poisons-harness.md (embedded/quiet
# background mode). The program self-reaps its server, so nothing lingers.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

fail() { echo "  [FAIL] $1"; exit 1; }

OUT="$(AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/quiet.ae" 2>&1)" \
    || { echo "$OUT"; fail "background server program errored"; }

echo "$OUT" | grep -q "BG-OK" || { echo "$OUT"; fail "background server did not serve"; }
if echo "$OUT" | grep -qE "Server running at|Press Ctrl\+C"; then
    echo "$OUT"
    fail "background server printed the interactive banner (should be quiet)"
fi

# #2672: stop joins the server's thread and leaves Winsock up, so a server
# can be stopped, freed and started again in one process, five times.
OUT2="$(AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/restart.ae" 2>&1)"     || { echo "$OUT2"; fail "restarting a background server errored"; }
echo "$OUT2" | grep -q "RESTART-OK" || { echo "$OUT2"; fail "a background server could not be started again after a stop"; }

echo "  [PASS] http_server_background_quiet: background server is silent, serves, stops cleanly, and starts again"
