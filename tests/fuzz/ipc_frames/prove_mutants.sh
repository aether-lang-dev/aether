#!/bin/sh
set -e

# Show the frame-reader harness finds a reader that breaks its contract.
# Each mutant is one edit to a copy of std/ipc/aether_ipc.c, compiled into
# the target exactly as run_fuzz_ipc_frames.sh compiles the real file, so
# the real source is untouched and the two builds carry the same coverage
# and sanitizers. A mutant counts as found only when the harness's model
# disagrees with it within FUZZ_SECONDS (default 300) and libFuzzer writes
# the input that did it as a crash-* file; that file is then replayed against
# the clean build, which must accept it. An OOM, a timeout, a leak or an
# exit for any other reason is a failure of the proof, not a find.
#
# M1: the "frame longer than max" check removed (the reader then allocates
#     and waits for what the header announces; the model expects a refusal).
# M2: a timeout inside a frame's header reported as the between-frames one
#     (the caller would keep using a stream that is out of step).
# M3: a frame of length 0 consumed as if its header had not been read.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
CLANG="${CLANG:-clang}"
FUZZ_SECONDS="${FUZZ_SECONDS:-300}"
SRC="$ROOT/std/ipc/aether_ipc.c"
OUT="${FUZZ_OUT:-$ROOT/build/fuzz/ipc_frames_mutants}"
mkdir -p "$OUT"

export ASAN_OPTIONS="${ASAN_OPTIONS:-malloc_context_size=8}"

AE_CFLAGS="$("$AE" cflags)"
# shellcheck disable=SC2086
AE_CC="$CLANG -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined -g -O1" \
    "$AE" build "$SCRIPT_DIR/fuzz_ipc_frames.ae" --with=extern,os --emit=obj -o "$OUT/harness_ae.o"

build_target() {
    # shellcheck disable=SC2086
    "$CLANG" -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -g -O1 -pthread \
        -DAETHER_HAS_FILESYSTEM=1 \
        "$SCRIPT_DIR/fuzz_ipc_frames.c" "$OUT/harness_ae.o" "$1" \
        $AE_CFLAGS -o "$2"
}

build_target "$SRC" "$OUT/fuzz_clean"

run_mutant() {
    name="$1"
    mutant_c="$2"
    grep -q "MUTANT_APPLIED" "$mutant_c" || { echo "FAIL: $name edit did not apply" >&2; exit 1; }
    build_target "$mutant_c" "$OUT/fuzz_$name"
    rm -f "$OUT/$name"-crash-* "$OUT/$name"-leak-* "$OUT/$name"-timeout-* "$OUT/$name"-oom-*
    rm -rf "$OUT/corpus_$name"
    mkdir -p "$OUT/corpus_$name"
    cp "$SCRIPT_DIR"/corpus/* "$OUT/corpus_$name/"
    echo "==> $name: fuzzing up to ${FUZZ_SECONDS}s, expecting the model to disagree..."
    set +e
    "$OUT/fuzz_$name" -dict="$SCRIPT_DIR/ipc_frames.dict" -max_len=8192 -rss_limit_mb=2048 \
        -timeout=10 -max_total_time="$FUZZ_SECONDS" -artifact_prefix="$OUT/$name-" \
        "$OUT/corpus_$name" > "$OUT/$name.log" 2>&1
    code=$?
    set -e
    if [ $code -eq 0 ]; then
        echo "FAIL: $name survived ${FUZZ_SECONDS}s: the harness did not notice it" >&2
        exit 1
    fi
    found="$(find "$OUT" -maxdepth 1 -name "$name-crash-*" | head -1)"
    if [ -z "$found" ]; then
        echo "FAIL: $name exited $code without a crash-* input (see $OUT/$name.log)" >&2
        exit 1
    fi
    if ! grep -q "deadly signal" "$OUT/$name.log"; then
        echo "FAIL: $name stopped on something other than the model's abort (see $OUT/$name.log)" >&2
        exit 1
    fi
    set +e
    "$OUT/fuzz_clean" "$found" > "$OUT/$name-replay.log" 2>&1
    replay=$?
    set -e
    if [ $replay -ne 0 ]; then
        echo "FAIL: the clean reader also fails on $found (exit $replay, see $OUT/$name-replay.log)" >&2
        exit 1
    fi
    echo "  [PASS] $name: the model disagreed (abort), reproducer $found, clean build accepts it"
}

# M1: drop the max check.
sed -e '/^_tuple_string_string ipc_read_frame_raw/,/^}/ s|if ((uint64_t)len > (uint64_t)max) {|if (0) { /* MUTANT_APPLIED */|' "$SRC" > "$OUT/m1.c"
run_mutant m1 "$OUT/m1.c"

# M2: report a timeout inside the header as the between-frames one.
sed -e '/^_tuple_string_string ipc_read_frame_raw/,/^}/ s|return ipc_read_failed(got == 0 ? IPC_TIMED_OUT_BETWEEN : IPC_TIMED_OUT_INSIDE);|return ipc_read_failed(IPC_TIMED_OUT_BETWEEN); /* MUTANT_APPLIED */|' "$SRC" > "$OUT/m2.c"
run_mutant m2 "$OUT/m2.c"

# M3: an empty frame answered as a between-frames timeout.
sed -e '/^_tuple_string_string ipc_read_frame_raw/,/^}/ s|if (len == 0) {|if (len == 0) { return ipc_read_failed(IPC_TIMED_OUT_BETWEEN); /* MUTANT_APPLIED */|' "$SRC" > "$OUT/m3.c"
run_mutant m3 "$OUT/m3.c"

echo "==> All mutants found."
