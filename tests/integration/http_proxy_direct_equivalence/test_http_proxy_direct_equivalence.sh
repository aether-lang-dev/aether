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
. "$ROOT/tests/lib/portable.sh"
AE="$ROOT/build/ae"
SRC="$ROOT/tests/integration/http_reverse_proxy/server.ae"

command -v curl >/dev/null 2>&1 || { echo "  [SKIP] curl not on PATH"; exit 0; }

. "$ROOT/tests/lib/wait_port.sh"
. "$ROOT/tests/lib/server_jobs.sh"

TMPDIR="$(mktemp -d)"
UP_PID=""; PX_PID=""
cleanup() {
    stop_server "$UP_PID"
    stop_server "$PX_PID"
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

if ! AETHER_HOME="$ROOT" "$AE" build "$SRC" -o "$TMPDIR/server" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] build:"; head -20 "$TMPDIR/build.log"; exit 1
fi

head_c 2048 /dev/urandom > "$TMPDIR/post.in" 2>/dev/null || \
    { i=0; while [ $i -lt 64 ]; do printf 'payload-0123456789abcdef'; i=$((i+1)); done > "$TMPDIR/post.in"; }

wait_ready() {
    pid="$1"; log="$2"; tag="$3"
    deadline=$(($(date +%s) + 15))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if grep -q '^READY ' "$log" 2>/dev/null; then return 0; fi
        kill -0 "$pid" 2>/dev/null || { echo "  [FAIL] $tag died:"; head -20 "$log"; exit 1; }
        sleep 0.05
    done
    echo "  [FAIL] $tag never READY:"; head -20 "$log"; exit 1
}

# One upstream for both captures. The echoed body names the upstream's port,
# so a second upstream on a second ephemeral port would differ from the first
# for a reason that has nothing to do with the two response paths.
AETHER_HOME="$ROOT" "$TMPDIR/server" upstream >"$TMPDIR/up.log" 2>&1 &
UP_PID=$!
wait_ready "$UP_PID" "$TMPDIR/up.log" upstream
UP_PORT=$(read_ready_port "$TMPDIR/up.log") || exit 1

# fix_length <header-file> <delta>: add delta to the file's Content-Length.
fix_length() {
    LC_ALL=C awk -v d="$2" '
        tolower(substr($0, 1, 15)) == "content-length:" {
            printf "%s %d\r\n", substr($0, 1, 15), substr($0, 16) + d; next }
        { print }' "$1" > "$1.norm" && mv "$1.norm" "$1"
}

# capture <label> — run the proxy and record every response shape.
capture() {
    label="$1"
    AETHER_PROXY_DIRECT="$2" AETHER_HOME="$ROOT" "$TMPDIR/server" proxy "$UP_PORT" \
        >"$TMPDIR/px.$label.log" 2>&1 &
    PX_PID=$!
    wait_ready "$PX_PID" "$TMPDIR/px.$label.log" "proxy ($label)"
    PX_PORT=$(read_ready_port "$TMPDIR/px.$label.log") || exit 1
    wait_port "$PX_PORT" || exit 1

    curl -s -o /dev/null --max-time 3 "http://127.0.0.1:$PX_PORT/echo" || {
        echo "  [FAIL] proxy never answered ($label)"; exit 1; }

    curl -s -D "$TMPDIR/$label.get.h"  --max-time 5 "http://127.0.0.1:$PX_PORT/echo"       -o "$TMPDIR/$label.get.b"
    curl -s -D "$TMPDIR/$label.post.h" --max-time 5 -X POST --data-binary "@$TMPDIR/post.in" \
                                                    "http://127.0.0.1:$PX_PORT/echo"       -o "$TMPDIR/$label.post.b"
    curl -s -D "$TMPDIR/$label.long.h" --max-time 5 "http://127.0.0.1:$PX_PORT/longheader" -o "$TMPDIR/$label.long.b"
    curl -s -D "$TMPDIR/$label.head.h" --max-time 5 -I "http://127.0.0.1:$PX_PORT/echo"    -o /dev/null
    # 64 KiB: large enough that the write does not complete in one call, which
    # is what exercises the accounting for a partially written body.
    curl -s -D "$TMPDIR/$label.big.h"  --max-time 10 "http://127.0.0.1:$PX_PORT/bigbody"  -o "$TMPDIR/$label.big.b"

    stop_server "$PX_PID"; PX_PID=""

    # The two captures are two proxy processes on two kernel-assigned ports,
    # and the upstream echoes X-Forwarded-Host, which names the port the
    # client dialled. That one number is the only byte that may legitimately
    # differ, so it is replaced in both captures and everything else is still
    # compared exactly.
    #
    # The echoed body holds that port, so its Content-Length does too: 117
    # bytes for a five-digit port and 116 for a four-digit one, and a kernel
    # whose ephemeral range starts below 10000 (OpenBSD's: 1024) hands out
    # both. The header is corrected by what replacing the port did to the
    # body, so the length is still compared, to the byte, against the body.
    for f in "$TMPDIR/$label".*.b; do
        [ -f "$f" ] || continue
        # LC_ALL=C: two of the captured bodies are binary, and BSD sed
        # refuses a byte sequence that is not valid in the ambient locale.
        old=$(wc -c < "$f" | tr -d ' ')
        LC_ALL=C sed "s/:$PX_PORT/:PROXY_PORT/g" "$f" > "$f.norm" && mv "$f.norm" "$f"
        new=$(wc -c < "$f" | tr -d ' ')
        h="${f%.b}.h"
        if [ "$new" != "$old" ] && [ -f "$h" ]; then
            fix_length "$h" $((new - old))
        fi
    done
    for f in "$TMPDIR/$label".*.h; do
        [ -f "$f" ] || continue
        LC_ALL=C sed "s/:$PX_PORT/:PROXY_PORT/g" "$f" > "$f.norm" && mv "$f.norm" "$f"
    done
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
for shape in get post long head big; do
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

[ "$fail" = "0" ] && echo "  [PASS] http_proxy_direct_equivalence: 5/5 shapes byte-identical (GET, POST, long header, HEAD, 64 KiB body)"
exit $fail
