#!/bin/sh
# std.guest (the host's side) driving stub_guest.ae (std.guest.child, the
# guest's side) in a real child process: mount, a call and its reply, a
# file granted as a descriptor and one denied, log levels, the guest's own
# lockdown, a polite cancel, a guest that ignores CANCEL (killed), a frame
# over the cap (refused) and the fail-closed spawn. On Linux the guest runs
# under sandbox.spawn_socket; elsewhere launch must refuse, and the rest
# run uncontained, asked for by name.
#
# Skips on Windows, where the socket channel is not implemented.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

# The platform from uname, never from the environment's OS (check_platform_env.py).
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*)
        echo "  [SKIP] std_guest: the std.ipc socket channel is POSIX-only"
        exit 0 ;;
esac

if [ ! -x "$AE" ]; then
    echo "  [SKIP] std_guest: ae not built"
    exit 0
fi
if [ "$(uname -s)" = Linux ] && [ ! -f "$ROOT/build/libaether_sandbox.so" ]; then
    echo "  [SKIP] std_guest: build/libaether_sandbox.so not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT
export AETHER_HOME="$ROOT"

for a in stub_guest host; do
    if ! "$AE" build "$SCRIPT_DIR/$a.ae" -o "$TMP/$a" > "$TMP/build.log" 2>&1; then
        echo "  [FAIL] std_guest: cannot build $a.ae"
        tail -20 "$TMP/build.log" | sed 's/^/    /'
        exit 1
    fi
done

if ! "$TMP/host" "$TMP/stub_guest" "$TMP" > "$TMP/out.log" 2>&1; then
    echo "  [FAIL] std_guest"
    tail -60 "$TMP/out.log" | sed 's/^/    /'
    exit 1
fi
if ! grep -q "17 passing" "$TMP/out.log" || grep -q "failing" "$TMP/out.log"; then
    echo "  [FAIL] std_guest: expected 17 passing, none failing"
    tail -60 "$TMP/out.log" | sed 's/^/    /'
    exit 1
fi
echo "  [PASS] std_guest: 17 specs, $(uname -s) ($(if [ "$(uname -s)" = Linux ]; then echo contained; else echo launch refused, the rest uncontained by name; fi))"
exit 0
