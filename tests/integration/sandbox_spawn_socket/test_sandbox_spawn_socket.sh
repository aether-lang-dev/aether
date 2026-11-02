#!/bin/sh
# sandbox.spawn_socket: os.run_socket's channel with spawn_sandboxed's
# containment (aether#3503).
#
#   - Linux: the child gets the socket at fd 3 (AETHER_IPC_KIND=socket), its
#     argv and its env, and its libc calls are held to the grants by the
#     preload. Without the preload library nothing starts (fail closed).
#   - macOS, FreeBSD: nothing can contain the child, so the call returns an
#     error and starts nothing (a marker file the child would make stays
#     absent).
#
# The kernel layer's own cases (Landlock, the widened seccomp filter,
# grant_runtime) are in test_sandbox_kernel_layer.sh beside this one.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] sandbox_spawn_socket: no socket channel on Windows (spawn_socket fails closed there)"
        exit 0 ;;
esac
if [ ! -x "$AE" ]; then
    echo "  [SKIP] sandbox_spawn_socket: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT
export AETHER_HOME="$ROOT"

if ! "$AE" build "$SCRIPT_DIR/host.ae" -o "$TMP/host" > "$TMP/build.log" 2>&1; then
    echo "  [FAIL] sandbox_spawn_socket: host build failed"
    tail -20 "$TMP/build.log" | sed 's/^/    /'
    exit 1
fi

fail() {
    echo "  [FAIL] sandbox_spawn_socket: $1"
    sed 's/^/    /' "$2"
    exit 1
}

case "$(uname -s)" in
    Linux) ;;
    *)
        # Fail closed: an error, no fd, no pid, and the program never ran.
        "$TMP/host" - /usr/bin/touch "$TMP/marker" > "$TMP/closed.out" 2>&1 || fail "host exited non-zero" "$TMP/closed.out"
        grep -q "^spawn: sandbox.spawn_socket: cannot contain a child on" "$TMP/closed.out" \
            || fail "expected spawn_socket to refuse on $(uname -s)" "$TMP/closed.out"
        grep -q "^fd=-1 pid=-1$" "$TMP/closed.out" || fail "expected fd=-1 pid=-1" "$TMP/closed.out"
        [ ! -e "$TMP/marker" ] || fail "the child ran although spawn_socket refused" "$TMP/closed.out"
        echo "  [PASS] sandbox_spawn_socket: refuses on $(uname -s) and starts nothing"
        exit 0 ;;
esac

if [ ! -f "$ROOT/build/libaether_sandbox.so" ]; then
    echo "  [SKIP] sandbox_spawn_socket: build/libaether_sandbox.so not built"
    exit 0
fi
if ! ${CC:-cc} -O1 -o "$TMP/probe" "$SCRIPT_DIR/probe.c" > "$TMP/cc.log" 2>&1; then
    fail "cannot build probe.c" "$TMP/cc.log"
fi
echo "greeting=from the fixture" > "$TMP/fixture.txt"

# The channel, argv, env and the preload, held to the grants: the fixture
# and the probe's own directory are granted, /etc/passwd is not.
"$TMP/host" "fs_read=$TMP/*,env=SPAWN_TEST" "$TMP/probe" hello "$TMP/fixture.txt" \
    > "$TMP/hello.out" 2>&1 || fail "host exited non-zero" "$TMP/hello.out"
for want in "spawn: ok" "channel fd=3 kind=socket" "argc=3 argv1=hello" "env SPAWN_TEST=hello" \
            "libc fopen /etc/passwd: refused" "libc fopen fixture: ALLOWED greeting=from the fixture" \
            "exit=0"; do
    grep -qF "$want" "$TMP/hello.out" || fail "expected '$want'" "$TMP/hello.out"
done
echo "  [PASS] sandbox_spawn_socket: socket at fd 3, argv, env, libc held to the grants"

# An env variable that is not granted reads as unset through the preload.
"$TMP/host" "fs_read=$TMP/*" "$TMP/probe" hello > "$TMP/noenv.out" 2>&1 || fail "host exited non-zero" "$TMP/noenv.out"
grep -qF "env SPAWN_TEST=(unset)" "$TMP/noenv.out" || fail "expected an ungranted env var to be hidden" "$TMP/noenv.out"
echo "  [PASS] sandbox_spawn_socket: an ungranted env var is hidden"

# Fail closed: no preload library, no child.
AETHER_SANDBOX_PRELOAD=/nonexistent/libaether_sandbox.so \
    "$TMP/host" - /usr/bin/touch "$TMP/marker" > "$TMP/nopreload.out" 2>&1 || fail "host exited non-zero" "$TMP/nopreload.out"
grep -q "^spawn: sandbox.spawn_socket: cannot find libaether_sandbox.so" "$TMP/nopreload.out" \
    || fail "expected a missing preload to refuse" "$TMP/nopreload.out"
[ ! -e "$TMP/marker" ] || fail "the child ran without its preload" "$TMP/nopreload.out"
echo "  [PASS] sandbox_spawn_socket: no preload library, nothing started"

# A program that does not exist is an error, not a child that exits 127.
"$TMP/host" - /nonexistent/program > "$TMP/noprog.out" 2>&1 || fail "host exited non-zero" "$TMP/noprog.out"
grep -q "^spawn: program not found" "$TMP/noprog.out" || fail "expected 'program not found'" "$TMP/noprog.out"
echo "  [PASS] sandbox_spawn_socket: a missing program is an error"
exit 0
