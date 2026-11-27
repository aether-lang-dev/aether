#!/bin/sh
# std.ipc shared buffers across processes: host.ae makes a buffer, passes its
# descriptor to child.ae over an os.run_socket channel, and each side reads
# what the other wrote into it. On Linux the same child runs again under
# sandbox.spawn_socket (the preload, Landlock and seccomp), granted nothing
# but reading its own directory: creating and mapping a buffer need no grant.
#
# Skips on Windows, where std.ipc passes no descriptors.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*)
        echo "  [SKIP] std_ipc_shared_buffer: std.ipc passes no descriptors on Windows"
        exit 0 ;;
esac
if [ ! -x "$AE" ]; then
    echo "  [SKIP] std_ipc_shared_buffer: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT
export AETHER_HOME="$ROOT"

fail() {
    echo "  [FAIL] std_ipc_shared_buffer: $1"
    sed 's/^/    /' "$2"
    exit 1
}

mkdir "$TMP/bin"
for a in host child; do
    "$AE" build "$SCRIPT_DIR/$a.ae" -o "$TMP/bin/$a" > "$TMP/build.log" 2>&1 || fail "cannot build $a.ae" "$TMP/build.log"
done

"$TMP/bin/host" plain "$TMP/bin/child" > "$TMP/plain.out" 2>&1 || fail "plain" "$TMP/plain.out"
grep -q "^PASS plain" "$TMP/plain.out" || fail "plain: no PASS" "$TMP/plain.out"
echo "  [PASS] std_ipc_shared_buffer: frames through a buffer a child maps"

if [ "$(uname -s)" != Linux ]; then
    echo "  [SKIP] std_ipc_shared_buffer: sandbox.spawn_socket contains a child on Linux only"
    exit 0
fi
if [ ! -f "$ROOT/build/libaether_sandbox.so" ]; then
    echo "  [SKIP] std_ipc_shared_buffer: build/libaether_sandbox.so not built"
    exit 0
fi
"$TMP/bin/host" sandboxed "$TMP/bin/child" > "$TMP/sandboxed.out" 2>&1 || fail "sandboxed" "$TMP/sandboxed.out"
grep -q "^PASS sandboxed" "$TMP/sandboxed.out" || fail "sandboxed: no PASS" "$TMP/sandboxed.out"
echo "  [PASS] std_ipc_shared_buffer: the same under sandbox.spawn_socket, with no grant"
exit 0
