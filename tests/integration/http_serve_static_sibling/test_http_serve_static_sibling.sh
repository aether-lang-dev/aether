#!/bin/sh
# #3747: http.serve_static checks that what it serves resolves inside its
# document root, and the check was a bare prefix match. A symlink in the
# root `.../www` that resolved into the sibling `.../www-private` passed it,
# since `.../www-private/secret.txt` starts with `.../www`. The prefix must
# now end at a path separator.
#
# Cases:
#   1. a plain file in the root                 -> 200
#   2. a symlink to a file inside the root      -> 200
#   3. a symlink to a sibling directory         -> 403
#   4. a symlink to a file in that sibling      -> 403
#
# Symlinks are the subject, so this runs where they are ordinary: not on
# Windows, where the containment check has no symlink resolution to escape.

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] http_serve_static_sibling: symlink containment, covered by the POSIX matrix"
        exit 0
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/wait_port.sh"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] http_serve_static_sibling: ae not built"; exit 0
fi
if ! command -v curl >/dev/null 2>&1; then
    echo "  [SKIP] curl not on PATH"; exit 0
fi

TMPDIR="$(mktemp -d)"
SRV_PID=""
cleanup() {
    stop_server "$SRV_PID"
    rm -rf "$TMPDIR"
}
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

mkdir "$TMPDIR/www" "$TMPDIR/www-private"
printf 'public\n' > "$TMPDIR/www/inside.txt"
printf 'secret\n' > "$TMPDIR/www-private/secret.txt"
ln -s inside.txt "$TMPDIR/www/alias.txt"
ln -s ../www-private "$TMPDIR/www/leak"
ln -s ../www-private/secret.txt "$TMPDIR/www/leak.txt"

AETHER_HOME="$ROOT" TEST_BASE="$TMPDIR/www" "$AE" run "$SCRIPT_DIR/server.ae" >"$TMPDIR/srv.log" 2>&1 &
SRV_PID=$!

deadline=$(($(date +%s) + 15))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if grep -q READY "$TMPDIR/srv.log" 2>/dev/null; then break; fi
    if ! kill -0 "$SRV_PID" 2>/dev/null; then
        echo "  [FAIL] server died:"; head -20 "$TMPDIR/srv.log"; exit 1
    fi
    sleep 0.1
done
PORT=$(read_ready_port "$TMPDIR/srv.log") || exit 1
wait_port "$PORT" || exit 1

# expect PATH STATUS [BODY]
expect() {
    got=$(curl -s -o "$TMPDIR/body" -w '%{http_code}' "http://127.0.0.1:$PORT$1")
    [ "$got" = "$2" ] || fail "$1: expected $2, got $got ($(cat "$TMPDIR/body"))"
    if [ -n "$3" ]; then
        [ "$(cat "$TMPDIR/body")" = "$3" ] || fail "$1: expected body '$3', got '$(cat "$TMPDIR/body")'"
    fi
    if grep -q secret "$TMPDIR/body"; then
        fail "$1: served the sibling directory's file"
    fi
    echo "  [PASS] $1 -> $got"
}

expect /inside.txt 200 public
expect /alias.txt 200 public
expect /leak/secret.txt 403
expect /leak.txt 403

echo "  [PASS] http_serve_static_sibling: a symlink into a sibling directory is refused (#3747)"
