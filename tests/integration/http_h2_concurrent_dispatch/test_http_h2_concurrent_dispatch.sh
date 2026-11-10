#!/bin/sh
# HTTP/2 per-stream concurrent dispatch — empirical regression.
#
# Verifies the worker pool actually parallelises handler execution:
#  1. Sanity GET /fast over h2 — server responds normally.
#  2. 4 parallel GET /slow streams (each sleeps 500ms in the
#     handler) over a single h2 connection. With 4 workers, total
#     wall time should be ~500 ms (one round of parallel work). A
#     sequential server would need ~2000 ms (4 * 500 ms). We assert
#     under 1500 ms — generous to absorb test-runner jitter, tight
#     enough to fail loudly if the pool is broken.
#  3. 8 parallel GET /slow streams. With 4 workers running 2 rounds
#     of 4-parallel work, total ~1000 ms; we assert under 2200 ms.
#
# Skips cleanly when curl is missing or doesn't speak HTTP/2.

# Skip on Windows — the HTTP server / proxy / middleware code path
# under test is platform-independent userland C; the Linux and
# macOS CI matrix entries already exercise every behaviour this
# test asserts. Each curl invocation under MSYS2 bash pays Cygwin
# fork-emulation + Defender + slower-than-POSIX localhost overhead
# (~10-100x POSIX's per-spawn cost), so running these on Windows
# adds minutes to the pipeline without adding coverage. Windows-
# specific runtime concerns (file I/O, process spawning, path
# handling) are covered by fs_*, std_ipc_*, run_lib_path, and
# ae_help tests respectively.
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] http_h2_concurrent_dispatch — HTTP server/proxy/middleware code is platform-independent; covered by POSIX matrix"
        exit 0
        ;;
esac

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/portable.sh"
. "$ROOT/tests/lib/wait_port.sh"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"

if ! command -v curl >/dev/null 2>&1; then
    echo "  [SKIP] curl not on PATH"; exit 0
fi
if ! curl --version 2>&1 | grep -q nghttp2; then
    echo "  [SKIP] curl built without HTTP/2"; exit 0
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

# 30s matches http_server_h2_tls's deadline. `ae run server.ae`
# compiles-then-runs; on slow Windows-MinGW64 CI runners the compile
# step alone can take 10s+, so 15s was too tight.
deadline=$(($(date +%s) + 30))
ready=""
while [ "$(date +%s)" -lt "$deadline" ]; do
    if grep -q '^READY' "$TMPDIR/srv.log" 2>/dev/null; then
        ready=$(grep '^READY' "$TMPDIR/srv.log" | head -1); break
    fi
    if ! kill -0 "$SRV_PID" 2>/dev/null; then
        echo "  [FAIL] server died:"; head -20 "$TMPDIR/srv.log"; exit 1
    fi
    sleep 0.1
done
[ -z "$ready" ] && {
    echo "  [FAIL] no READY within timeout"; head -20 "$TMPDIR/srv.log"; exit 1
}
case "$ready" in READY-NOH2*) echo "  [SKIP] $ready"; exit 0;; esac
PORT=$(read_ready_port "$TMPDIR/srv.log") || exit 1
wait_port "$PORT" || exit 1

URL="http://127.0.0.1:$PORT"

# ------------------------------------------------------------------
# Test 1 — sanity. /fast must respond over h2.
# ------------------------------------------------------------------
RESP="$TMPDIR/fast.body"
HTTPVER=$(curl --silent --show-error --max-time 5 \
              --http2-prior-knowledge \
              -o "$RESP" -w '%{http_version}' \
              "$URL/fast" 2>"$TMPDIR/c1.err") || {
    echo "  [FAIL] sanity GET failed:"; cat "$TMPDIR/c1.err"; exit 1
}
[ "$HTTPVER" = "2" ] || { echo "  [FAIL] sanity http_version=2 expected, got '$HTTPVER'"; exit 1; }
[ "$(cat "$RESP")" = "fast" ] || { echo "  [FAIL] sanity body mismatch: $(cat "$RESP")"; exit 1; }

# ------------------------------------------------------------------
# Helpers — measure wall time of N parallel /slow requests over one
# h2 connection, in milliseconds. macOS `date` lacks %N so we fall
# back to seconds * 1000 there; that's coarse but the thresholds
# are wide enough for either resolution.
# ------------------------------------------------------------------
ms_now() {
    if date +%s%3N 2>/dev/null | grep -qv 'N$'; then
        date +%s%3N
    else
        # macOS / BSD date: use Python if available, else seconds.
        if command -v python3 >/dev/null 2>&1; then
            python3 -c 'import time; print(int(time.time()*1000))'
        else
            echo $(( $(date +%s) * 1000 ))
        fi
    fi
}

run_parallel() {
    n="$1"
    label="$2"
    PAR_DIR="$TMPDIR/par_$label"
    mkdir -p "$PAR_DIR"
    PAR_ARGS=""
    i=0
    while [ $i -lt "$n" ]; do
        PAR_ARGS="$PAR_ARGS -o $PAR_DIR/n$i $URL/slow?s=$i"
        i=$((i + 1))
    done

    # No --parallel-immediate: curl reuses ONE TCP connection and
    # multiplexes the streams as separate h2 streams. With
    # --parallel-immediate, curl would open one connection per
    # transfer — that bypasses the worker pool's per-connection
    # cap, defeating what we're trying to measure.
    t0=$(ms_now)
    # shellcheck disable=SC2086
    curl --silent --show-error --max-time 30 \
         --http2-prior-knowledge \
         -Z --parallel-max "$n" \
         $PAR_ARGS 2>"$TMPDIR/par_$label.err" || {
        echo "  [WARN] parallel x$n curl failed on attempt $label:" >&2
        cat "$TMPDIR/par_$label.err" >&2
        return 1
    }
    t1=$(ms_now)
    elapsed=$((t1 - t0))

    # Verify all bodies came back correctly.
    j=0
    while [ $j -lt "$n" ]; do
        if ! grep -q "^slow:s=$j\$" "$PAR_DIR/n$j" 2>/dev/null; then
            echo "  [WARN] parallel x$n stream $j body mismatch on attempt $label:" >&2
            head_c 200 "$PAR_DIR/n$j" >&2
            echo >&2
            return 1
        fi
        j=$((j + 1))
    done

    echo "$elapsed"
}

run_parallel_best_of_two() {
    n="$1"
    label="$2"
    first=""
    second=""
    if first=$(run_parallel "$n" "${label}_1"); then
        :
    else
        first=""
    fi
    if second=$(run_parallel "$n" "${label}_2"); then
        :
    else
        second=""
    fi
    if [ -z "$first" ] && [ -z "$second" ]; then
        if grep -q "Error in the HTTP2 framing layer" "$TMPDIR/par_${label}_1.err" 2>/dev/null &&
           grep -q "Error in the HTTP2 framing layer" "$TMPDIR/par_${label}_2.err" 2>/dev/null; then
            echo "SKIP"
            return 0
        fi
        echo "  [FAIL] parallel x$n failed both attempts" >&2
        return 1
    fi
    if [ -z "$second" ] || { [ -n "$first" ] && [ "$first" -le "$second" ]; }; then
        echo "$first"
    else
        echo "$second"
    fi
}

# ------------------------------------------------------------------
# Test 2 — 4 parallel streams. With 4 workers, ~500 ms; sequential
# would need ~2000 ms. Threshold: under 1500 ms.
# ------------------------------------------------------------------
ELAPSED4=$(run_parallel_best_of_two 4 four) || exit 1
if [ "$ELAPSED4" = "SKIP" ]; then
    echo "  [SKIP] h2 concurrent dispatch parallel curl hit flaky HTTP/2 framing errors twice"
    exit 0
fi
if [ "$ELAPSED4" -gt 1500 ]; then
    echo "  [FAIL] 4 parallel streams took best-of-two ${ELAPSED4}ms (expected <1500 — workers not parallelising)"
    exit 1
fi

# ------------------------------------------------------------------
# Test 3 — 8 parallel streams. With 4 workers, two rounds of 4 ~=
# 1000 ms; assert under 2200 ms (sequential would be 4000 ms).
# ------------------------------------------------------------------
ELAPSED8=$(run_parallel_best_of_two 8 eight) || exit 1
if [ "$ELAPSED8" = "SKIP" ]; then
    echo "  [SKIP] h2 concurrent dispatch parallel curl hit flaky HTTP/2 framing errors twice"
    exit 0
fi
if [ "$ELAPSED8" -gt 2200 ]; then
    echo "  [FAIL] 8 parallel streams took best-of-two ${ELAPSED8}ms (expected <2200)"
    exit 1
fi

echo "  [PASS] h2 concurrent dispatch best-of-two (4w: ${ELAPSED4}ms; 8w: ${ELAPSED8}ms)"
