#!/bin/sh
set -e

# Run the coverage-guided fuzz harness for std.ipc's frame reader
# (read_frame / read_frame_timeout), driven over a socket pair.
#
# Builds the harness with:
#   clang -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -g -O1 against $(ae cflags)
# and runs it for FUZZ_SECONDS (default 60) on a copy of the seed corpus.
# Linux only: the reader's timeout form needs poll() and os.socketpair.
# Exits non-zero on a crash, leak, timeout or oracle failure, naming the
# reproducer.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

FUZZ_SECONDS="${FUZZ_SECONDS:-60}"
CORPUS_DIR="$SCRIPT_DIR/corpus"
DICT_FILE="$SCRIPT_DIR/ipc_frames.dict"

if [ ! -x "$AE" ]; then
    echo "Error: $AE not built. Run 'make' first." >&2
    exit 1
fi

CLANG="${CLANG:-clang}"
if ! command -v "$CLANG" >/dev/null 2>&1; then
    echo "Error: clang ($CLANG) not found." >&2
    exit 1
fi

# Check if the compiler supports -fsanitize=fuzzer
CHECK_TMP="$(mktemp -d)"
check_supported=1
cat << 'EOF' > "$CHECK_TMP/check_fuzzer.c"
#include <stdint.h>
#include <stddef.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) { return 0; }
EOF

if ! "$CLANG" -fsanitize=fuzzer "$CHECK_TMP/check_fuzzer.c" -o "$CHECK_TMP/check_fuzzer" >/dev/null 2>&1; then
    check_supported=0
fi
rm -rf "$CHECK_TMP"

if [ "$check_supported" -eq 0 ]; then
    echo "Error: compiler ($CLANG) lacks libFuzzer support (-fsanitize=fuzzer failed to link)." >&2
    echo "Install libclang-rt-<version>-dev (e.g. libclang-rt-18-dev on Ubuntu) or use a clang build with compiler-rt built-in." >&2
    exit 1
fi

# Everything goes under build/fuzz/ipc_frames and stays there: the harness binary
# (to reproduce with), the working corpus (a copy of the seeds, which grows
# run to run) and any crash-*, leak-*, timeout-* or oom-* reproducer.
OUT="${FUZZ_OUT:-$ROOT/build/fuzz/ipc_frames}"
mkdir -p "$OUT/corpus"

echo "==> Building $OUT/fuzz_ipc_frames..."
# The Aether side of the harness carries coverage and sanitizer
# instrumentation; the reader itself is C in the prebuilt runtime, so the
# fuzzer steers by the harness's model and by what the runtime returns. UBSan halts rather than
# printing and carrying on, so undefined behaviour fails the run.
AE_CC="$CLANG -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined -g -O1" \
    "$AE" build "$SCRIPT_DIR/fuzz_ipc_frames.ae" --with=extern,os --emit=obj -o "$OUT/fuzz_ipc_frames_ae.o"

AE_CFLAGS="$("$AE" cflags)"

# shellcheck disable=SC2086
"$CLANG" -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -g -O1 \
    "$SCRIPT_DIR/fuzz_ipc_frames.c" "$OUT/fuzz_ipc_frames_ae.o" \
    $AE_CFLAGS \
    -o "$OUT/fuzz_ipc_frames"

cp "$CORPUS_DIR"/* "$OUT/corpus/"

# ASan keeps a stack trace for every allocation and never frees one. The
# decoder recurses once per level, so with the default 30 frames nearly
# every nesting shape is a new trace, and RSS climbed past -rss_limit_mb
# (2 GB) in 30 minutes with no leak in the program. 8 frames still name the
# allocating function and stay flat. A caller's ASAN_OPTIONS wins.
export ASAN_OPTIONS="${ASAN_OPTIONS:-malloc_context_size=8}"

echo "==> Fuzzing for ${FUZZ_SECONDS}s..."
set +e
"$OUT/fuzz_ipc_frames" \
    -dict="$DICT_FILE" \
    -max_len=16384 \
    -rss_limit_mb=2048 \
    -timeout=10 \
    -max_total_time="$FUZZ_SECONDS" \
    -artifact_prefix="$OUT/" \
    -print_final_stats=1 \
    "$OUT/corpus"
EXIT_CODE=$?
set -e

if [ $EXIT_CODE -ne 0 ]; then
    echo "FAIL: fuzz_ipc_frames exited $EXIT_CODE" >&2
    for repro in "$OUT"/crash-* "$OUT"/leak-* "$OUT"/timeout-* "$OUT"/oom-*; do
        [ -f "$repro" ] && echo "  reproducer: $repro  (rerun: $OUT/fuzz_ipc_frames $repro)" >&2
    done
    exit $EXIT_CODE
fi

echo "==> No crash, leak, timeout or oracle failure in ${FUZZ_SECONDS}s."
