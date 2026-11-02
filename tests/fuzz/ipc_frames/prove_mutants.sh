#!/bin/sh
set -e

# Show the frame-reader harness finds a reader that breaks its contract.
# Each mutant is one edit to a copy of std/ipc/aether_ipc.c, compiled into a
# separate runtime object that is linked ahead of libaether's, so the real
# source and build are untouched. The harness's model must then disagree
# with the reader within FUZZ_SECONDS (default 300) and abort.
#
# M1: the "frame longer than max" check removed (the reader then allocates
#     and waits for what the header announces; the model expects a refusal).
# M2: a timeout inside a frame reported as the between-frames one (the
#     caller would keep using a stream that is out of step).
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

run_mutant() {
    name="$1"
    mutant_c="$2"
    grep -q "MUTANT_APPLIED" "$mutant_c" || { echo "FAIL: $name edit did not apply" >&2; exit 1; }
    # shellcheck disable=SC2086
    "$CLANG" -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -g -O1 \
        -DAETHER_HAS_FILESYSTEM=1 \
        "$SCRIPT_DIR/fuzz_ipc_frames.c" "$OUT/harness_ae.o" "$mutant_c" \
        $AE_CFLAGS -o "$OUT/fuzz_$name"
    mkdir -p "$OUT/corpus_$name"
    cp "$SCRIPT_DIR"/corpus/* "$OUT/corpus_$name/"
    echo "==> $name: fuzzing up to ${FUZZ_SECONDS}s, expecting the model to disagree..."
    set +e
    "$OUT/fuzz_$name" -dict="$SCRIPT_DIR/ipc_frames.dict" -max_len=16384 -rss_limit_mb=2048 \
        -timeout=10 -max_total_time="$FUZZ_SECONDS" -artifact_prefix="$OUT/$name-" \
        "$OUT/corpus_$name" > "$OUT/$name.log" 2>&1
    code=$?
    set -e
    if [ $code -eq 0 ]; then
        echo "FAIL: $name survived ${FUZZ_SECONDS}s: the harness did not notice it" >&2
        exit 1
    fi
    found="$(ls "$OUT"/$name-crash-* 2>/dev/null | head -1)"
    reason="sanitizer report"
    grep -q "deadly signal" "$OUT/$name.log" && reason="the model disagreed (abort)"
    echo "  [PASS] $name: $reason, reproducer ${found:-see $OUT/$name.log}"
}

# M1: drop the max check.
sed -e '/^_tuple_string_string ipc_read_frame_raw/,/^}/ s|if ((uint64_t)len > (uint64_t)max) {|if (0) { /* MUTANT_APPLIED */|' "$SRC" > "$OUT/m1.c"
run_mutant m1 "$OUT/m1.c"

# M2: report every timeout as the between-frames one.
sed -e '/^_tuple_string_string ipc_read_frame_raw/,/^}/ s|return ipc_read_failed(got == 0 ? IPC_TIMED_OUT_BETWEEN : IPC_TIMED_OUT_INSIDE);|return ipc_read_failed(IPC_TIMED_OUT_BETWEEN); /* MUTANT_APPLIED */|' "$SRC" > "$OUT/m2.c"
run_mutant m2 "$OUT/m2.c"

# M3: an empty frame answered as a between-frames timeout.
sed -e '/^_tuple_string_string ipc_read_frame_raw/,/^}/ s|if (len == 0) {|if (len == 0) { return ipc_read_failed(IPC_TIMED_OUT_BETWEEN); /* MUTANT_APPLIED */|' "$SRC" > "$OUT/m3.c"
run_mutant m3 "$OUT/m3.c"

echo "==> All mutants found."
