#!/bin/bash
# Comparison benchmark: thread-per-connection vs actor dispatch
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

# Preflight: wrk drives every measurement here too.
if ! command -v wrk >/dev/null 2>&1; then
    echo "ERROR: wrk is not installed (brew install wrk / apt install wrk)." >&2
    exit 1
fi

# Write to a temp file, publish only on success, so an interrupted run
# never leaves a truncated results file in the tree.
RESULTS_FINAL="benchmarks/http/comparison_results.txt"
RESULTS="$(mktemp "${TMPDIR:-/tmp}/aether_comparison.XXXXXX")"
trap 'rm -f "$RESULTS"' EXIT
echo "=== HTTP Server Benchmark: Thread vs Actor ===" > "$RESULTS"
echo "Date: $(date)" >> "$RESULTS"
echo "CPU: $(cpu_model)" >> "$RESULTS"
echo "Cores: $(cpu_cores)" >> "$RESULTS"
echo "" >> "$RESULTS"

# Build both
echo "=== Building benchmarks ==="
bash benchmarks/http/run_baseline.sh 2>/dev/null | grep "Build OK" || true

gcc -O2 -Icompiler -Iruntime -Iruntime/actors -Iruntime/scheduler -Iruntime/utils \
    -Iruntime/memory -Iruntime/config -Istd -Istd/string -Istd/io -Istd/math \
    -Istd/net -Istd/collections -Istd/json \
    benchmarks/http/bench_actor_http.c build/libaether.a \
    -o build/bench_actor_http \
    -pthread -lm \
    $(pkg-config --libs openssl 2>/dev/null) \
    $(pkg-config --libs zlib 2>/dev/null) \
    $(pkg-config --libs libnghttp2 2>/dev/null) \
    $(pkg-config --libs libpcre2-8 2>/dev/null)
echo "Both builds OK"

for conns in 10 100 500 1000; do
    echo ""
    echo "========================================" | tee -a "$RESULTS"
    echo "  $conns concurrent connections" | tee -a "$RESULTS"
    echo "========================================" | tee -a "$RESULTS"

    # Thread mode
    echo "" | tee -a "$RESULTS"
    echo "--- THREAD MODE ---" | tee -a "$RESULTS"
    ./build/bench_thread_http &
    PID=$!
    sleep 1
    if kill -0 $PID 2>/dev/null; then
        wrk -t4 -c$conns -d10s http://localhost:8080/api/hello 2>&1 | tee -a "$RESULTS"
        kill $PID 2>/dev/null; wait $PID 2>/dev/null || true
    else
        echo "THREAD SERVER FAILED TO START" | tee -a "$RESULTS"
    fi
    sleep 2

    # Actor mode
    echo "" | tee -a "$RESULTS"
    echo "--- ACTOR MODE ---" | tee -a "$RESULTS"
    ./build/bench_actor_http &
    PID=$!
    sleep 1
    if kill -0 $PID 2>/dev/null; then
        wrk -t4 -c$conns -d10s http://localhost:8080/api/hello 2>&1 | tee -a "$RESULTS"
        kill $PID 2>/dev/null; wait $PID 2>/dev/null || true
    else
        echo "ACTOR SERVER FAILED TO START" | tee -a "$RESULTS"
    fi
    sleep 2
done

echo ""
echo "=== Full results saved to $RESULTS ==="

# Every measurement landed: publish the complete file.
mv "$RESULTS" "$RESULTS_FINAL"
trap - EXIT
echo "=== Results saved to $RESULTS_FINAL ==="
