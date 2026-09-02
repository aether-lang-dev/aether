#!/bin/sh
# std.http.client must not replay credentials across a PORT change (#1741).
#
# The client dropped Authorization, Cookie and Proxy-Authorization when a
# redirect changed host, and decided that on the host name alone: the parsed
# port and TLS flag were read and then never compared. A hop from
# 127.0.0.1:18221 to 127.0.0.1:18222 therefore handed the caller's bearer
# token to a different service on the same machine, owned by whoever holds
# that port.
#
# An origin is scheme + host + port together, which is the rule curl applies.
# Two servers, same host name, different ports, plus a same-origin control:
# a client that stripped unconditionally would satisfy the first assertion
# and fail the second.

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] http_redirect_cross_port — POSIX sockets; covered by the POSIX matrix"
        exit 0
        ;;
esac

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] http_redirect_cross_port: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
ORIGIN_PID=""
TARGET_PID=""
cleanup() {
    for p in $ORIGIN_PID $TARGET_PID; do
        kill "$p" 2>/dev/null || true
        wait "$p" 2>/dev/null || true
    done
    rm -rf "$TMP"
}
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; [ -f "$TMP/driver.out" ] && sed 's/^/        /' "$TMP/driver.out" | head -6; exit 1; }

"$AE" build "$SCRIPT_DIR/origin_server.ae" -o "$TMP/origin" >"$TMP/b1.log" 2>&1 \
    || { sed 's/^/        /' "$TMP/b1.log" | head -8; fail "origin_server.ae did not build"; }
"$AE" build "$SCRIPT_DIR/target_server.ae" -o "$TMP/target" >"$TMP/b2.log" 2>&1 \
    || { sed 's/^/        /' "$TMP/b2.log" | head -8; fail "target_server.ae did not build"; }
"$AE" build "$SCRIPT_DIR/driver.ae" -o "$TMP/driver" >"$TMP/b3.log" 2>&1 \
    || { sed 's/^/        /' "$TMP/b3.log" | head -8; fail "driver.ae did not build"; }

"$TMP/origin" >"$TMP/origin.log" 2>&1 &
ORIGIN_PID=$!
"$TMP/target" >"$TMP/target.log" 2>&1 &
TARGET_PID=$!

deadline=$(($(date +%s) + 15))
until curl -s -o /dev/null --max-time 1 "http://127.0.0.1:18221/landing" 2>/dev/null \
   && curl -s -o /dev/null --max-time 1 "http://127.0.0.1:18222/landing" 2>/dev/null; do
    [ "$(date +%s)" -ge "$deadline" ] && fail "servers never accepted connections"
    sleep 0.1
done

"$TMP/driver" >"$TMP/driver.out" 2>&1 || fail "driver exited non-zero"
grep -q "^PASS:" "$TMP/driver.out" || fail "driver did not report PASS"

echo "  [PASS] http_redirect_cross_port: credentials dropped on a port change, kept within one origin"
