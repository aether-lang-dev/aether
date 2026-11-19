#!/bin/sh
# #3760: on Windows, http.serve_static resolved the requested path with
# _fullpath, which removes `.` and `..` but follows no junction or symlink.
# A junction in the document root pointing outside it passed the containment
# check, and the file behind it was served. The opened file's final path
# (GetFinalPathNameByHandle) is now compared with the root's.
#
# Cases, root `www` beside `private`:
#   1. a plain file in the root                          -> 200
#   2. a file through a junction to a directory in the root -> 200
#   3. a file through a junction to `private`            -> 403
#   4. a root that is itself a junction to `www`         -> 200
#
# Junctions (`mklink /J`, no privilege needed) are the Windows subject; the
# POSIX symlink cases are http_serve_static_sibling's.

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) ;;
    *)
        echo "  [SKIP] http_serve_static_junction: Windows junctions; POSIX symlinks are http_serve_static_sibling's"
        exit 0
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/wait_port.sh"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] http_serve_static_junction: ae not built"; exit 0
fi
if ! command -v curl >/dev/null 2>&1; then
    echo "  [SKIP] curl not on PATH"; exit 0
fi

TMPDIR="$(mktemp -d)"
SRV_PID=""
cleanup() {
    stop_server "$SRV_PID"
    # A junction is removed with rmdir, which leaves its target alone; rm -rf
    # could walk into the target first.
    for j in "$TMPDIR/www/leak" "$TMPDIR/www/alias" "$TMPDIR/rootlink"; do
        [ -d "$j" ] && cmd //c rmdir "$(cygpath -w "$j")" >/dev/null 2>&1
    done
    rm -rf "$TMPDIR" || :
    return 0
}
trap cleanup EXIT
fail() { echo "  [FAIL] http_serve_static_junction: $1"; exit 1; }

mkdir -p "$TMPDIR/www/sub" "$TMPDIR/private"
printf 'public\n' > "$TMPDIR/www/inside.txt"
printf 'nested\n' > "$TMPDIR/www/sub/nested.txt"
printf 'secret\n' > "$TMPDIR/private/secret.txt"
junction() {
    cmd //c mklink //J "$(cygpath -w "$1")" "$(cygpath -w "$2")" >/dev/null 2>&1
}
if ! junction "$TMPDIR/www/leak" "$TMPDIR/private"; then
    echo "  [SKIP] http_serve_static_junction: mklink /J failed here"
    exit 0
fi
junction "$TMPDIR/www/alias" "$TMPDIR/www/sub" || fail "mklink /J alias"
junction "$TMPDIR/rootlink" "$TMPDIR/www" || fail "mklink /J rootlink"

AE_BIN="$TMPDIR/srv"
AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/server.ae" -o "$AE_BIN" >"$TMPDIR/build.log" 2>&1 \
    || { sed -n '1,10p' "$TMPDIR/build.log"; fail "server did not build"; }

# serve BASE: start the server on document root BASE; sets PORT.
serve() {
    stop_server "$SRV_PID"
    TEST_BASE="$(cygpath -m "$1")" "$AE_BIN" >"$TMPDIR/srv.log" 2>&1 &
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
}

# expect PATH STATUS [BODY]
expect() {
    got=$(curl -s -o "$TMPDIR/body" -w '%{http_code}' "http://127.0.0.1:$PORT$1")
    body=$(tr -d '\r' < "$TMPDIR/body")
    [ "$got" = "$2" ] || fail "$1: expected $2, got $got ($body)"
    if [ -n "$3" ]; then
        [ "$body" = "$3" ] || fail "$1: expected body '$3', got '$body'"
    fi
    case "$body" in
        *secret*) fail "$1: served the file behind the junction out of the root" ;;
    esac
    echo "  [PASS] $1 -> $got"
}

serve "$TMPDIR/www"
expect /inside.txt 200 public
expect /alias/nested.txt 200 nested
expect /leak/secret.txt 403

serve "$TMPDIR/rootlink"
expect /inside.txt 200 public
expect /leak/secret.txt 403

echo "  [PASS] http_serve_static_junction: a junction out of the document root is refused (#3760)"
