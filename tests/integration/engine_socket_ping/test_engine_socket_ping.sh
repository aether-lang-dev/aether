#!/bin/sh
# An optional check against a real container engine: GET /_ping over the
# Docker or Podman Engine API's Unix socket must answer 200 "OK", buffered
# and streamed (main.ae). The engine is found, never started: DOCKER_HOST
# when it names a unix:// socket, then a running Podman machine's API
# socket, then the rootless and rootful Podman sockets, then
# /var/run/docker.sock. With none reachable the test skips.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=engine_socket_ping

[ -x "$AE" ] || { echo "  [SKIP] $NAME: $AE not built"; exit 0; }

sock=""
case "${DOCKER_HOST:-}" in
    unix://*) sock="${DOCKER_HOST#unix://}" ;;
esac
if [ -z "$sock" ] && command -v podman >/dev/null 2>&1 && [ "$(uname -s)" = Darwin ]; then
    p=$(podman machine inspect --format '{{.ConnectionInfo.PodmanSocket.Path}}' 2>/dev/null | head -1)
    [ -n "$p" ] && [ -S "$p" ] && sock="$p"
fi
for p in "${XDG_RUNTIME_DIR:-/nonexistent}/podman/podman.sock" /run/podman/podman.sock /var/run/docker.sock; do
    [ -n "$sock" ] && break
    [ -S "$p" ] && [ -w "$p" ] && sock="$p"
done
if [ -z "$sock" ] || [ ! -S "$sock" ]; then
    echo "  [SKIP] $NAME: no Docker or Podman API socket reachable"
    exit 0
fi

out=$( cd "$SCRIPT_DIR" && AE_ENGINE_SOCKET="$sock" "$AE" run main.ae 2>&1 )
rc=$?
if [ "$rc" -ne 0 ] || printf '%s\n' "$out" | grep -q 'FAIL'; then
    echo "  [FAIL] $NAME: over $sock (exit $rc)"
    printf '%s\n' "$out" | head -20 | sed 's/^/          /'
    exit 1
fi
if ! printf '%s\n' "$out" | grep -q '^PASS'; then
    echo "  [FAIL] $NAME: no PASS line"
    printf '%s\n' "$out" | head -20 | sed 's/^/          /'
    exit 1
fi
echo "  [PASS] $NAME: $(printf '%s\n' "$out" | grep '^PASS' | sed 's/^PASS: //') ($sock)"
exit 0
