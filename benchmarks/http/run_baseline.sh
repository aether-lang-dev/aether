#!/bin/bash
# Baseline benchmark for thread-per-connection HTTP server
# Run from aether/ directory: bash benchmarks/http/run_baseline.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT_DIR"

cpu_model() {
    if command -v lscpu >/dev/null 2>&1; then
        lscpu | grep 'Model name' | sed 's/.*: *//'
    elif [ "$(uname -s)" = "Darwin" ]; then
        sysctl -n machdep.cpu.brand_string
    else
        uname -m
    fi
}

cpu_cores() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    else
        sysctl -n hw.ncpu 2>/dev/null || echo "unknown"
    fi
}

# Preflight. wrk drives every measurement, so a missing binary would
# otherwise be discovered only after the build and the server start,
# leaving a half-written results file behind.
if ! command -v wrk >/dev/null 2>&1; then
    echo "ERROR: wrk is not installed (brew install wrk / apt install wrk)." >&2
    echo "       It generates the load for this benchmark; nothing to measure without it." >&2
    exit 1
fi

echo "=== Building baseline benchmark ==="
# Link the precompiled stdlib archive rather than naming individual
# sources. A hand-maintained source list silently rots as the runtime
# grows; `make stdlib` is the authoritative set.
make stdlib >/dev/null
gcc -O2 -Icompiler -Iruntime -Iruntime/actors -Iruntime/scheduler -Iruntime/utils \
    -Iruntime/memory -Iruntime/config -Istd -Istd/string -Istd/io -Istd/math \
    -Istd/net -Istd/collections -Istd/json \
    benchmarks/http/bench_thread_http.c build/libaether.a \
    -o build/bench_thread_http \
    -pthread -lm \
    $(pkg-config --libs openssl 2>/dev/null) \
    $(pkg-config --libs zlib 2>/dev/null) \
    $(pkg-config --libs libnghttp2 2>/dev/null) \
    $(pkg-config --libs libpcre2-8 2>/dev/null)
echo "Build OK"

# Write to a temp file and publish only on success. A run that dies
# partway (server fails to start, wrk missing, interrupted) used to
# leave a truncated results file that reads as a broken benchmark to
# anyone browsing the repo, and one such stub was committed that way.
RESULTS_FILE="benchmarks/http/baseline_results.txt"
RESULTS_TMP="$(mktemp "${TMPDIR:-/tmp}/aether_baseline.XXXXXX")"
cleanup() {
    rm -f "$RESULTS_TMP"
    if [ -n "${SERVER_PID:-}" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

echo "=== HTTP Baseline Benchmark (thread-per-connection) ===" > "$RESULTS_TMP"
echo "Date: $(date)" >> "$RESULTS_TMP"
echo "CPU: $(cpu_model)" >> "$RESULTS_TMP"
echo "Cores: $(cpu_cores)" >> "$RESULTS_TMP"
echo "" >> "$RESULTS_TMP"

# Start server in background
./build/bench_thread_http &
SERVER_PID=$!
sleep 1

# Verify server is running. This is the check that actually fired when
# the committed stub was produced, so say WHY rather than just that it
# failed: a stale listener on 8080 is the usual cause.
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "ERROR: the benchmark server exited immediately after launch." >&2
    echo "       Most often port 8080 is already in use; check with" >&2
    echo "         lsof -i :8080    (or: ss -ltnp | grep 8080)" >&2
    echo "       then re-run. No results file was written." >&2
    exit 1
fi

echo "=== Running benchmarks ==="
for conns in 10 100 500 1000; do
    echo ""
    echo "--- $conns concurrent connections ---"
    echo "--- $conns concurrent connections ---" >> "$RESULTS_TMP"
    wrk -t4 -c$conns -d10s http://localhost:8080/api/hello 2>&1 | tee -a "$RESULTS_TMP"
    echo "" >> "$RESULTS_TMP"
    sleep 2
done

# Cleanup
kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null || true
SERVER_PID=""

# Every measurement landed: publish the complete file.
mv "$RESULTS_TMP" "$RESULTS_FILE"

echo ""
echo "=== Results saved to $RESULTS_FILE ==="
cat "$RESULTS_FILE"
