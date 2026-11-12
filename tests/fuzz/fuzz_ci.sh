#!/bin/sh

# Every fuzz target for FUZZ_SECONDS each (default 60), one after another:
# what the fuzz workflow runs, as `make fuzz-ci`. A target that fails does
# not stop the rest, so one run reports all of them; the script exits non-zero
# after the last, naming each that failed. Every target's binary, working
# corpus and any crash-*, leak-*, timeout-* or oom-* reproducer stay in
# build/fuzz/<target>/.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export FUZZ_SECONDS="${FUZZ_SECONDS:-60}"

failed=""
run() {
    name="$1"
    shift
    echo "=== fuzz $name (${FUZZ_SECONDS}s)"
    if "$@"; then
        echo "=== fuzz $name: passed"
    else
        echo "=== fuzz $name: FAILED"
        failed="$failed $name"
    fi
}

run cbor sh "$SCRIPT_DIR/cbor/run_fuzz_cbor.sh"
run ipc_frames sh "$SCRIPT_DIR/ipc_frames/run_fuzz_ipc_frames.sh"
for target in http_head http_chunked url json; do
    run "$target" sh "$SCRIPT_DIR/run_fuzz_c.sh" "$target"
done

if [ -n "$failed" ]; then
    echo "FAIL: fuzz targets that failed:$failed (reproducers in build/fuzz/<target>/)" >&2
    exit 1
fi
echo "All fuzz targets passed, ${FUZZ_SECONDS}s each."
