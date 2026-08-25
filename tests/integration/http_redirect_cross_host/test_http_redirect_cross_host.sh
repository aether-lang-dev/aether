#!/bin/sh
# std.http.client must not replay credentials across a host change (#1739).
#
# http_strip_cross_host_headers drops Authorization, Cookie and
# Proxy-Authorization when a redirect hop changes authority. That is the
# behaviour that stops a bearer token reaching a host the user never
# authorised, and it had no runtime coverage: the existing redirect test
# asserts the API surface (set_follow_redirects returns "", rejects -1) and
# never follows a hop.
#
# One server, two hops on the same port under different host names, and the
# same-name control is what gives the cross-host assertion meaning: a client
# that dropped the headers unconditionally would satisfy the first assertion
# and fail the second.
#
# The rule the client implements compares the parsed host and nothing else,
# so the port is held equal across both hops to isolate it. A change of port
# alone does NOT strip today; that is filed separately.

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] http_redirect_cross_host — POSIX sockets; covered by the POSIX matrix"
        exit 0
        ;;
esac

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] http_redirect_cross_host: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
ORIGIN_PID=""
TARGET_PID=""   # unused; kept so cleanup stays uniform
cleanup() {
    for p in $ORIGIN_PID $TARGET_PID; do
        kill "$p" 2>/dev/null || true
        wait "$p" 2>/dev/null || true
    done
    rm -rf "$TMP"
}
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; [ -f "$TMP/driver.out" ] && sed 's/^/        /' "$TMP/driver.out" | head -6; exit 1; }

"$AE" build "$SCRIPT_DIR/server.ae" -o "$TMP/server" >"$TMP/build.log" 2>&1 \
    || { sed 's/^/        /' "$TMP/build.log" | head -8; fail "server.ae did not build"; }
"$AE" build "$SCRIPT_DIR/driver.ae" -o "$TMP/driver" >"$TMP/build2.log" 2>&1 \
    || { sed 's/^/        /' "$TMP/build2.log" | head -8; fail "driver.ae did not build"; }

"$TMP/server" >"$TMP/server.log" 2>&1 &
ORIGIN_PID=$!

deadline=$(($(date +%s) + 15))
until curl -s -o /dev/null --max-time 1 "http://127.0.0.1:18211/landing" 2>/dev/null; do
    [ "$(date +%s)" -ge "$deadline" ] && fail "server never accepted connections"
    sleep 0.1
done

"$TMP/driver" >"$TMP/driver.out" 2>&1 || fail "driver exited non-zero"
if grep -q "^SKIP:" "$TMP/driver.out"; then
    echo "  [SKIP] http_redirect_cross_host: $(sed -n 's/^SKIP: //p' "$TMP/driver.out" | head -1)"
    exit 0
fi
grep -q "^PASS:" "$TMP/driver.out" || fail "driver did not report PASS"

echo "  [PASS] http_redirect_cross_host: credentials dropped on a host change, kept without one"
