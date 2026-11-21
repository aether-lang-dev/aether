#!/bin/sh
# The host calls the guest, end to end: std.guest driving
# tests/integration/std_guest/stub_guest.ae in a real child through READY,
# INVOKE, RETURN, RAISE and END (docs/design/guest-host-calls-guest.md):
# notify then fetch, the JSON values both ways, and the safety rows
# (slow, silent, oversize, raises, re-entry while called, export flood,
# malformed list, after end, a frame while idle, a guest sending INVOKE).
# On Linux the guest runs under sandbox.spawn_socket; elsewhere it runs
# uncontained, asked for by name.
#
# Skips on Windows, where the socket channel is not implemented.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

# The platform from uname, never from the environment's OS (check_platform_env.py).
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*)
        echo "  [SKIP] std_guest_invoke: the std.ipc socket channel is POSIX-only"
        exit 0 ;;
esac

if [ ! -x "$AE" ]; then
    echo "  [SKIP] std_guest_invoke: ae not built"
    exit 0
fi
if [ "$(uname -s)" = Linux ] && [ ! -f "$ROOT/build/libaether_sandbox.so" ]; then
    echo "  [SKIP] std_guest_invoke: build/libaether_sandbox.so not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT
export AETHER_HOME="$ROOT"

for src in "$ROOT/tests/integration/std_guest/stub_guest.ae" "$SCRIPT_DIR/invoke_host.ae"; do
    out="$TMP/$(basename "$src" .ae)"
    if ! "$AE" build "$src" -o "$out" > "$TMP/build.log" 2>&1; then
        echo "  [FAIL] std_guest_invoke: cannot build $(basename "$src")"
        tail -20 "$TMP/build.log" | sed 's/^/    /'
        exit 1
    fi
done

if ! "$TMP/invoke_host" "$TMP/stub_guest" "$TMP" > "$TMP/out.log" 2>&1; then
    echo "  [FAIL] std_guest_invoke"
    tail -60 "$TMP/out.log" | sed 's/^/    /'
    exit 1
fi
if ! grep -q "14 passing" "$TMP/out.log" || grep -q "failing" "$TMP/out.log"; then
    echo "  [FAIL] std_guest_invoke: expected 14 passing, none failing"
    tail -60 "$TMP/out.log" | sed 's/^/    /'
    exit 1
fi
echo "  [PASS] std_guest_invoke: 14 specs, $(uname -s) ($(if [ "$(uname -s)" = Linux ]; then echo contained; else echo uncontained by name; fi))"
exit 0
