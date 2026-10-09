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

echo "  [PASS] http_server_background_quiet: background server is silent, serves, stops cleanly"
