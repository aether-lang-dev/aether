#!/bin/sh
# WebSocket client: bounded recv, readiness poll, and the raw descriptor.
#
# A multiplexed protocol over one socket (WebDriver-BiDi: many commands in
# flight, each awaited by id, plus unsolicited events) needs ONE reader that
# routes frames to an id-table or an event queue. Built on a blocking
# ws_recv, that reader has to own a thread -- which is exactly what a demux
# driven from 24 language bindings' own event loops is trying to avoid.
# (asks/ws-client-nonblocking-recv-for-bidi-demux.md)
#
# The interesting assertions are the negative ones. Any recv can return a
# frame that is already there; the value of these calls is that they come
# back EMPTY, promptly, when the socket is quiet -- and that a poll which
# says "readable" does not eat the frame it is reporting. The peer therefore
# stalls ~900ms before its first reply, to create a window that is genuinely
# quiet rather than merely fast.
#
# Deliberately not asserted: elapsed wall-clock. "returned within 100ms +/-
# 10" is a flake generator on a loaded runner. The assertions are on return
# values and ordering, with one generous upper bound that fails only if a
# frame never arrives at all.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] ws_client_nonblocking: ae not built"; exit 0; }

TMP="$(mktemp -d)"
SRV_PID=""
cleanup() {
    # `|| :` on every line: under `set -e` a failing command inside a trap
    # aborts the rest of the handler, which would leak $TMP and leave a
    # non-zero status behind an already-printed [PASS].
    if [ -n "$SRV_PID" ]; then kill "$SRV_PID" 2>/dev/null || :; fi
    rm -rf "$TMP" || :
    return 0
}
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

if command -v timeout >/dev/null 2>&1; then
    TIMEOUT="timeout 60"
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT="gtimeout 60"
else
    TIMEOUT=""
fi

"$AE" build "$SCRIPT_DIR/wsserver.ae" -o "$TMP/wssrv" >"$TMP/b1.log" 2>&1 \
    || { sed -n '1,10p' "$TMP/b1.log"; fail "server did not build"; }
"$AE" build "$SCRIPT_DIR/wsclient.ae" -o "$TMP/wscli" >"$TMP/b2.log" 2>&1 \
    || { sed -n '1,10p' "$TMP/b2.log"; fail "client did not build"; }

"$TMP/wssrv" > "$TMP/srv.log" 2>&1 &
SRV_PID=$!

# Poll for the listener rather than sleeping a fixed amount.
i=0
while [ "$i" -lt 60 ]; do
    grep -q READY "$TMP/srv.log" 2>/dev/null && break
    sleep 0.1
    i=$((i + 1))
done
grep -q READY "$TMP/srv.log" 2>/dev/null || {
    sed -n '1,10p' "$TMP/srv.log"
    fail "ws server never became READY"
}

OUT=$($TIMEOUT "$TMP/wscli" 2>&1) || {
    echo "$OUT" | sed 's/^/         /'
    fail "client exited non-zero"
}
case "$OUT" in
    *"PASS: ws non-blocking recv / poll / fd"*) ;;
    *) echo "$OUT" | sed 's/^/         /'; fail "client did not report PASS" ;;
esac

echo "  [PASS] ws_client_nonblocking: bounded recv returns empty, poll does not consume, fd exposed"
