#!/bin/sh
# #625 regression: http.get_query_param returns the value for the
# REQUESTED key, not the last key's value, when the query has 2+
# parameters. The previous implementation parsed with strstr on every
# call and returned a pointer into a function-local static buffer —
# two sequential calls aliased to the second's value.

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] http_query_param_multi — HTTP server is platform-independent; covered by POSIX matrix"
        exit 0
        ;;
esac

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/wait_port.sh"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"

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

AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/server.ae" >"$TMPDIR/srv.log" 2>&1 &
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

BASE="http://127.0.0.1:$PORT"

# Test 1 — the regression. Three params; each must come back distinct.
RESP=$(curl --silent --show-error --max-time 5 "$BASE/multi?alpha=AAA&beta=BBB&gamma=CCC") || {
    echo "  [FAIL] curl multi failed"; exit 1
}
EXPECT="a=[AAA] b=[BBB] c=[CCC]"
if [ "$RESP" != "$EXPECT" ]; then
    echo "  [FAIL] multi-param query: expected '$EXPECT', got '$RESP'"
    echo "  (pre-fix bug returned a=[CCC] b=[CCC] c=[CCC] due to static-buffer aliasing)"
    exit 1
fi

# Test 2 — single-param query still works (sanity that we didn't break the easy case).
RESP=$(curl --silent --show-error --max-time 5 "$BASE/single?only=ONE") || {
    echo "  [FAIL] curl single failed"; exit 1
}
[ "$RESP" = "only=[ONE]" ] || {
    echo "  [FAIL] single-param query: expected 'only=[ONE]', got '$RESP'"; exit 1
}

# Test 3 — asking for a key that isn't present returns null/absent.
RESP=$(curl --silent --show-error --max-time 5 "$BASE/missing?something=else") || {
    echo "  [FAIL] curl missing failed"; exit 1
}
[ "$RESP" = "nope=[<null>]" ] || {
    echo "  [FAIL] missing-key query: expected 'nope=[<null>]', got '$RESP'"; exit 1
}

# Test 4 — key whose name is a substring of another key (the original
# strstr() boundary check was the only thing keeping this from
# misfiring; the new parser uses exact strcmp so the issue can't
# come back). `?aa=1&aaa=2` asking for "aa" must return 1, not 2.
RESP=$(curl --silent --show-error --max-time 5 "$BASE/multi?aa=1&aaa=2&beta=B") || {
    echo "  [FAIL] curl substring failed"; exit 1
}
# alpha/beta/gamma handler: only beta is present here, alpha and gamma null.
EXPECT="a=[<null>] b=[B] c=[<null>]"
[ "$RESP" = "$EXPECT" ] || {
    echo "  [FAIL] substring-key query: expected '$EXPECT', got '$RESP'"; exit 1
}

# Test 5 — get_query_param is the raw value; query_param decodes it, '+'
# as a space, and matches a decoded key. A malformed escape is an error.
RESP=$(curl --silent --show-error --max-time 5 "$BASE/decoded?q=a%20b+c%2Bd&a%20b=1") || {
    echo "  [FAIL] curl decoded failed"; exit 1
}
EXPECT="raw=[a%20b+c%2Bd] dec=[a b c+d] err=[] spaced=[1]"
[ "$RESP" = "$EXPECT" ] || {
    echo "  [FAIL] decoded query: expected '$EXPECT', got '$RESP'"; exit 1
}
RESP=$(curl --silent --show-error --max-time 5 "$BASE/decoded?q=%zz") || {
    echo "  [FAIL] curl malformed failed"; exit 1
}
EXPECT="raw=[%zz] dec=[] err=[malformed encoding] spaced=[]"
[ "$RESP" = "$EXPECT" ] || {
    echo "  [FAIL] malformed query: expected '$EXPECT', got '$RESP'"; exit 1
}

echo "  [PASS] http_query_param_multi: per-key values stable across calls (#625), raw and decoded"
