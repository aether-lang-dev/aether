#!/bin/sh
# A proxied response answered from the upstream's own bytes must be the same
# bytes the copying path would have produced.
#
# The fast path rebuilds the response head itself rather than serialising a
# response object, so it can disagree with the copying path in ways no
# single-path test would show: it did, on HEAD, where the copying path states
# the length of the body it is sending (none) and passing the upstream's
# header through would have claimed a body that never arrives.
#
# So the comparison is the test: the same requests through the same proxy with
# the fast path on and off, diffed byte for byte, headers and body.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
SRC="$ROOT/tests/integration/http_reverse_proxy/server.ae"

command -v curl >/dev/null 2>&1 || { echo "  [SKIP] curl not on PATH"; exit 0; }

TMPDIR="$(mktemp -d)"
UP_PID=""; PX_PID=""
cleanup() {
    [ -n "$UP_PID" ] && { kill "$UP_PID" 2>/dev/null || true; wait "$UP_PID" 2>/dev/null || true; }
    [ -n "$PX_PID" ] && { kill "$PX_PID" 2>/dev/null || true; wait "$PX_PID" 2>/dev/null || true; }
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

if ! AETHER_HOME="$ROOT" "$AE" build "$SRC" -o "$TMPDIR/server" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] build:"; head -20 "$TMPDIR/build.log"; exit 1
fi

head -c 2048 /dev/urandom > "$TMPDIR/post.in" 2>/dev/null || \
    { i=0; while [ $i -lt 64 ]; do printf 'payload-0123456789abcdef'; i=$((i+1)); done > "$TMPDIR/post.in"; }

# capture <label> — run the pair and record every response shape.
capture() {
    label="$1"
    AETHER_HOME="$ROOT" "$TMPDIR/server" upstream >"$TMPDIR/up.$label.log" 2>&1 &
    UP_PID=$!
    AETHER_PROXY_DIRECT="$2" AETHER_HOME="$ROOT" "$TMPDIR/server" proxy >"$TMPDIR/px.$label.log" 2>&1 &
    PX_PID=$!

    deadline=$(($(date +%s) + 15))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        curl -s -o /dev/null --max-time 1 "http://127.0.0.1:19000/echo" 2>/dev/null && break
        sleep 0.2
    done
    curl -s -o /dev/null --max-time 3 "http://127.0.0.1:19000/echo" || {
        echo "  [FAIL] proxy never answered ($label)"; exit 1; }

    curl -s -D "$TMPDIR/$label.get.h"  --max-time 5 "http://127.0.0.1:19000/echo"       -o "$TMPDIR/$label.get.b"
    curl -s -D "$TMPDIR/$label.post.h" --max-time 5 -X POST --data-binary "@$TMPDIR/post.in" \
                                                    "http://127.0.0.1:19000/echo"       -o "$TMPDIR/$label.post.b"
    curl -s -D "$TMPDIR/$label.long.h" --max-time 5 "http://127.0.0.1:19000/longheader" -o "$TMPDIR/$label.long.b"
    curl -s -D "$TMPDIR/$label.head.h" --max-time 5 -I "http://127.0.0.1:19000/echo"    -o /dev/null

    kill "$UP_PID" 2>/dev/null || true; wait "$UP_PID" 2>/dev/null || true; UP_PID=""
    kill "$PX_PID" 2>/dev/null || true; wait "$PX_PID" 2>/dev/null || true; PX_PID=""
    sleep 0.3
}

capture direct 1
capture copied 0

# Byte-comparison without diffutils: cksum is POSIX and MSYS2 has no cmp.
same() {   # same <file-a> <file-b>
    a=$(cksum < "$1" | awk '{print $1, $2}')
    b=$(cksum < "$2" | awk '{print $1, $2}')
    [ "$a" = "$b" ]
}

fail=0
for shape in get post long head; do
    same "$TMPDIR/direct.$shape.h" "$TMPDIR/copied.$shape.h" || {
        echo "  [FAIL] $shape: headers differ between the two paths"
        echo "    direct:"; sed 's/^/      /' "$TMPDIR/direct.$shape.h"
        echo "    copied:"; sed 's/^/      /' "$TMPDIR/copied.$shape.h"
        fail=1
    }
    if [ -f "$TMPDIR/direct.$shape.b" ]; then
        same "$TMPDIR/direct.$shape.b" "$TMPDIR/copied.$shape.b" || {
            echo "  [FAIL] $shape: bodies differ between the two paths"; fail=1; }
    fi
done

# A response has to have actually gone through the fast path, or this compares
# the copying path with itself and passes for the wrong reason.
grep -qi 'X-Upstream-Tag' "$TMPDIR/direct.get.h" || {
    echo "  [FAIL] the proxied response did not carry the upstream's headers"; fail=1; }

[ "$fail" = "0" ] && echo "  [PASS] http_proxy_direct_equivalence: 4/4 shapes byte-identical (GET, POST, long header, HEAD)"
exit $fail
