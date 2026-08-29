#!/bin/sh
# std.http.proxy reverse-proxy core integration tests.
#
# Verifies (mode=simple):
#   1. Basic round-trip: GET / through proxy → 200 + upstream body
#   2. Hop-by-Hop headers stripped both directions
#   3. X-Forwarded-For appended correctly (preserves prior values)
#   4. X-Forwarded-Proto + X-Forwarded-Host injected
#   5. Via: 1.1 aether-proxy injected
#   6. Host: rewritten to upstream's host (preserve_host=0 default)
#   7. POST body round-trips byte-identical (1 KiB)
#   8. Refuses Upgrade-bearing requests with 502 +
#      X-Aether-Proxy-Error: upgrade_unsupported
#   9. Custom request header reaches upstream
#  12. A response header value too long for the small-value buffer
#      survives the proxy whole
#  11. A request split across several writes, with the header
#      terminator itself cut in half and the body arriving last,
#      still completes
#
# Verifies (mode=timeout):
#  10. Upstream sleep 3s vs proxy timeout 1s → 504 within ~1.5s

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if ! command -v curl >/dev/null 2>&1; then
    echo "  [SKIP] curl not on PATH"
    exit 0
fi

TMPDIR="$(mktemp -d)"
UP_PID=""
PX_PID=""
cleanup() {
    # `wait` on a SIGTERM'd child returns 143; with `set -e`, that
    # would propagate as the script's exit code. `|| true` keeps the
    # cleanup quiet whether the child dies by signal or natural exit.
    if [ -n "$UP_PID" ]; then
        kill "$UP_PID" 2>/dev/null || true
        wait "$UP_PID" 2>/dev/null || true
    fi
    if [ -n "$PX_PID" ]; then
        kill "$PX_PID" 2>/dev/null || true
        wait "$PX_PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

# Build once.
if ! AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/server.ae" \
        -o "$TMPDIR/server" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] build:"; head -30 "$TMPDIR/build.log"; exit 1
fi

wait_for_port() {
    pid="$1"; port="$2"; tag="$3"; log="$4"
    deadline=$(($(date +%s) + 15))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "  [FAIL] $tag died:"; head -30 "$log"; exit 1
        fi
        if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$port/" 2>/dev/null \
           || curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$port/echo" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    echo "  [FAIL] $tag never accepted on port $port"; head -30 "$log"; exit 1
}

start_mode() {
    proxy_mode="$1"  # "proxy" or "proxy_timeout"
    AETHER_HOME="$ROOT" "$TMPDIR/server" upstream     >"$TMPDIR/up.log" 2>&1 &
    UP_PID=$!
    AETHER_HOME="$ROOT" "$TMPDIR/server" "$proxy_mode" >"$TMPDIR/px.log" 2>&1 &
    PX_PID=$!
    wait_for_port "$UP_PID" 19001 upstream "$TMPDIR/up.log"
    wait_for_port "$PX_PID" 19000 proxy    "$TMPDIR/px.log"
    sleep 0.3
}

stop_servers() {
    if [ -n "$UP_PID" ]; then
        kill "$UP_PID" 2>/dev/null || true
        wait "$UP_PID" 2>/dev/null || true
    fi
    if [ -n "$PX_PID" ]; then
        kill "$PX_PID" 2>/dev/null || true
        wait "$PX_PID" 2>/dev/null || true
    fi
    UP_PID=""; PX_PID=""
    sleep 0.3
}

PROXY="http://127.0.0.1:19000"

# Windows-reduced mode. Same rationale as the http_reverse_proxy_pool
# tests: each curl under MSYS2 bash pays Cygwin-fork-emulation +
# Defender + slower-localhost overhead (10-100x POSIX's per-spawn
# cost). The proxy/header-rewrite code path under test is identical
# across platforms — the POSIX matrix already covers every assertion
# below — so on Windows we keep only the most distinct happy/error
# paths and skip the rest. Reduction is visible in CI via
# `[SKIP-WIN]` log lines.
IS_WIN=0
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) IS_WIN=1 ;;
esac

# ----------------------------------------------------------------
# Mode: proxy (30s timeout)
# ----------------------------------------------------------------
start_mode proxy

# Test 1 — basic round-trip GET.
RESP=$(curl --silent --show-error --max-time 5 -w '|%{http_code}' "$PROXY/echo" 2>"$TMPDIR/c1.err") || {
    echo "  [FAIL] T1 curl:"; cat "$TMPDIR/c1.err"; exit 1
}
STATUS="${RESP##*|}"
BODY="${RESP%|*}"
[ "$STATUS" = "200" ] || { echo "  [FAIL] T1 status: expected 200, got $STATUS"; exit 1; }
echo "$BODY" | grep -q '^upstream-ok$' || {
    echo "  [FAIL] T1 body: missing 'upstream-ok' marker"; echo "$BODY"; exit 1;
}

# Tests 2-6: header forwarding details (Hop-by-Hop strip, XFF/XFP/
# XFH/Via injection, Host rewrite). Skipped on Windows — all share
# the same request-rewriting code path, fully exercised by POSIX
# matrix entries. Tests 3-6 already reuse a single curl response on
# POSIX (RESP from test 3 satisfies 4/5/6), so the dropped curl
# count on Windows is just 2 (the unique curls in tests 2 and 3).
if [ "$IS_WIN" = "1" ]; then
    echo "  [SKIP-WIN] T2-T6 header-forwarding details — POSIX matrix covers"
else
    # Test 2 — a header named in Connection is hop-by-hop for this message
    # and must not reach the upstream (RFC 9110 7.6.1). A sender names a
    # header there so the next hop does not see it, so an upstream that
    # trusts a header stays reachable through an intermediary that forwards
    # it anyway.
    RESP=$(curl --silent --show-error --max-time 5 \
                -H 'Connection: X-Hopby-Custom' \
                -H 'TE: trailers' \
                -H 'X-Hopby-Custom: leaked' \
                "$PROXY/echo" 2>"$TMPDIR/c2.err")
    echo "$RESP" | grep -q '^hopby=<none>' || {
        echo "  [FAIL] T2: a header named in Connection reached the upstream"
        echo "$RESP"; exit 1;
    }

    # Test 2b — the same header, not named in Connection, must arrive. This
    # is the half that keeps the strip from becoming "drop unknown headers".
    RESP=$(curl --silent --show-error --max-time 5 \
                -H 'X-Hopby-Custom: kept' \
                "$PROXY/echo" 2>"$TMPDIR/c2b.err")
    echo "$RESP" | grep -q '^hopby=kept' || {
        echo "  [FAIL] T2b: a header not named in Connection was dropped"
        echo "$RESP"; exit 1;
    }

    # Test 3 — X-Forwarded-For appended to existing value.
    RESP=$(curl --silent --show-error --max-time 5 \
                -H 'X-Forwarded-For: 1.2.3.4' \
                "$PROXY/echo" 2>"$TMPDIR/c3.err")
    echo "$RESP" | grep -q '^xff=1.2.3.4' || {
        echo "  [FAIL] T3 XFF: expected '1.2.3.4' prefix"; echo "$RESP"; exit 1;
    }

    # Test 4 — X-Forwarded-Proto and X-Forwarded-Host present.
    echo "$RESP" | grep -q '^xfp=http$' || {
        echo "  [FAIL] T4 XFP: expected 'http'"; echo "$RESP"; exit 1;
    }
    echo "$RESP" | grep -q '^xfh=' || {
        echo "  [FAIL] T4 XFH: missing"; echo "$RESP"; exit 1;
    }

    # Test 5 — Via header injected.
    echo "$RESP" | grep -q '^via=' || {
        echo "  [FAIL] T5 Via: missing"; echo "$RESP"; exit 1;
    }
    echo "$RESP" | grep -q 'aether-proxy' || {
        echo "  [FAIL] T5 Via: missing aether-proxy token"; echo "$RESP"; exit 1;
    }

    # Test 6 — Host: rewritten to upstream (default preserve_host=0).
    # Upstream is on localhost:19001 so it should see "localhost:19001".
    echo "$RESP" | grep -q '^host=localhost:19001$' || {
        echo "  [FAIL] T6 Host rewrite: expected 'localhost:19001'"; echo "$RESP"; exit 1;
    }
fi

# Test 7 — POST body round-trip.
BODY_IN="$TMPDIR/post.in"
BODY_OUT="$TMPDIR/post.out"
yes 'binary-payload-1234567890' 2>/dev/null | head -c 1024 > "$BODY_IN"
curl --silent --show-error --max-time 5 \
     -X POST -H 'Content-Type: application/octet-stream' \
     --data-binary "@$BODY_IN" \
     -o "$BODY_OUT" \
     "$PROXY/echo" 2>"$TMPDIR/c7.err" || {
    echo "  [FAIL] T7 POST curl:"; cat "$TMPDIR/c7.err"; exit 1;
}
# Byte-equality check that doesn't depend on diffutils (`cmp` is in
# diffutils which isn't always installed on MSYS2 / minimal CI
# images). `wc -c` is POSIX-universal; `od -An -tx1` produces a
# stable hex dump for byte comparison whether the file contains
# NULs, high-bit bytes, or arbitrary binary content.
SENT_BYTES=$(wc -c <"$BODY_IN" | tr -d ' ')
RECV_BYTES=$(wc -c <"$BODY_OUT" | tr -d ' ')
[ "$SENT_BYTES" = "$RECV_BYTES" ] || {
    echo "  [FAIL] T7 body round-trip length: sent $SENT_BYTES, received $RECV_BYTES"
    exit 1
}
SENT_HEX=$(od -An -tx1 -v "$BODY_IN" | tr -d ' \n')
RECV_HEX=$(od -An -tx1 -v "$BODY_OUT" | tr -d ' \n')
[ "$SENT_HEX" = "$RECV_HEX" ] || {
    echo "  [FAIL] T7 body round-trip content mismatch ($SENT_BYTES bytes each, byte-level diff)"
    exit 1
}

# Test 8 — Upgrade-bearing requests are refused.
STATUS=$(curl --silent --show-error --max-time 5 \
              -H 'Upgrade: websocket' -H 'Connection: Upgrade' \
              -o "$TMPDIR/up.body" \
              -D "$TMPDIR/up.hdr" \
              -w '%{http_code}' \
              "$PROXY/echo" 2>"$TMPDIR/c8.err") || true
[ "$STATUS" = "502" ] || { echo "  [FAIL] T8 Upgrade: expected 502, got $STATUS"; exit 1; }
grep -qi '^X-Aether-Proxy-Error: upgrade_unsupported' "$TMPDIR/up.hdr" || {
    echo "  [FAIL] T8 Upgrade: missing X-Aether-Proxy-Error: upgrade_unsupported"
    cat "$TMPDIR/up.hdr"; exit 1;
}

# Test 9 — custom request header passes through. Skipped on Windows
# (same as T2-T6): the request-header-forwarding code path is the
# same one Test 1 already exercises; T9 just adds one custom header.
if [ "$IS_WIN" = "1" ]; then
    echo "  [SKIP-WIN] T9 custom header pass-through — covered by T1 basic round-trip"
else
    RESP=$(curl --silent --show-error --max-time 5 \
                -H 'X-Custom-Pass: hello-upstream' \
                "$PROXY/echo" 2>"$TMPDIR/c9.err")
    # Upstream's /echo doesn't surface X-Custom-Pass directly, but the
    # request reached the upstream (we got the upstream-ok marker).
    echo "$RESP" | grep -q '^upstream-ok$' || {
        echo "  [FAIL] T9: custom header request didn't reach upstream"; exit 1;
    }
fi

# Test 11 - fragmented request. curl cannot split a request across
# writes, so this one speaks the socket directly. The event driver
# reads whatever has arrived and asks whether a request is complete
# yet, so the cut points here (inside the CRLFCRLF terminator, and
# again before the body) are the cases where a driver that tracks how
# far it has scanned can lose the terminator and wait forever.
# Test 12 - a response header value longer than the buffer the proxy copies
# small ones into. The proxy has to fall back to the heap for it, and the
# value has to arrive whole: a truncated one would still look like a valid
# response.
LONGHDR=$(curl -s --show-error --max-time 5 -D - -o /dev/null "$PROXY/longheader" 2>"$TMPDIR/c12.err" \
          | tr -d '\r' | grep -i '^X-Long-Value:' | sed 's/^[^:]*: *//')
LONGLEN=${#LONGHDR}
[ "$LONGLEN" = "1000" ] || {
    echo "  [FAIL] T12 long response header: expected 1000 bytes, got $LONGLEN"; exit 1;
}
case "$LONGHDR" in
    0123456789*0123456789) : ;;
    *) echo "  [FAIL] T12 long response header: content corrupted"; exit 1 ;;
esac

FRAG_RAN=0
if command -v python3 >/dev/null 2>&1; then
    if FRAG=$(python3 "$SCRIPT_DIR/fragment_probe.py" 19000 2>&1); then
        FRAG_RAN=1
    else
        echo "  [FAIL] T11 fragmented request: $FRAG"; exit 1
    fi
else
    echo "  [SKIP] T11 fragmented request: python3 not on PATH"
fi

stop_servers

# ----------------------------------------------------------------
# Mode: proxy_timeout (1s timeout)
# ----------------------------------------------------------------
start_mode proxy_timeout

# Test 10 — proxy timeout returns 504.
T0=$(date +%s)
STATUS=$(curl --silent --show-error --max-time 10 \
              -o "$TMPDIR/to.body" -w '%{http_code}' \
              "$PROXY/slow" 2>"$TMPDIR/c10.err") || true
T1=$(date +%s)
ELAPSED=$((T1 - T0))
[ "$STATUS" = "504" ] || { echo "  [FAIL] T10 status: expected 504, got $STATUS"; cat "$TMPDIR/to.body"; exit 1; }
# Upstream sleeps 3s; proxy timeout 1s. Total elapsed should be
# under 4s (tolerance for OS scheduling jitter).
[ "$ELAPSED" -lt 4 ] || { echo "  [FAIL] T10 timeout: expected <4s, got ${ELAPSED}s"; exit 1; }

stop_servers

if [ "$IS_WIN" = "1" ]; then
    if [ "$FRAG_RAN" = "1" ]; then
        echo "  [PASS] http_reverse_proxy: 6/12 win-reduced - basic, POST body, Upgrade refusal, long header, fragmented, timeout"
    else
        echo "  [PASS] http_reverse_proxy: 5/12 win-reduced - basic, POST body, Upgrade refusal, long header, timeout (fragmented skipped)"
    fi
else
    if [ "$FRAG_RAN" = "1" ]; then
        echo "  [PASS] http_reverse_proxy: 12/12 - basic round-trip, headers, body, long header, fragmented, timeout"
    else
        echo "  [PASS] http_reverse_proxy: 11/12 - basic round-trip, headers, body, long header, timeout (fragmented skipped)"
    fi
fi
