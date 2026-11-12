#!/bin/sh
set -e

# Build and run one of the C-parser fuzz targets:
#
#   http_head     the HTTP/1 request head on the server's path (tests/fuzz/http_head)
#   http_chunked  the chunked body framing and decoder (tests/fuzz/http_chunked)
#   url           the client's URL and Location parsers (tests/fuzz/url)
#   json          the JSON parser and serializer (tests/fuzz/json)
#
#   sh tests/fuzz/run_fuzz_c.sh <target>
#
# Builds the harness and the std sources it tests with
#   clang -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -g -O1
# so libFuzzer steers by the parsers' own branches and the sanitizers watch
# them, links the rest of the runtime from build/libaether.a, and runs it for
# FUZZ_SECONDS (default 60) on a copy of the seed corpus with the target's
# dictionary. Exits non-zero on a crash, leak, timeout or property failure,
# naming the reproducer.
#
# FUZZ_REPLAY=1 builds the same harness with $CC (default cc) and no
# libFuzzer, linked to tests/fuzz/replay_main.c, and runs every seed once
# (plus any files or directories named in FUZZ_REPLAY_INPUTS). It works where
# clang has no libFuzzer, Windows among them, and finds nothing new.
#
# The Makefile's fuzz-<target> rules pass the flags the std sources are
# compiled with (FUZZ_STD_CFLAGS); run by hand, the script takes them from
# `make -s print-fuzz-std-cflags`.

TARGET="$1"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

case "$TARGET" in
    http_head)    SOURCES="std/net/aether_http.c std/net/aether_http_server.c"; MAX_LEN=8192 ;;
    http_chunked) SOURCES="std/net/aether_http.c"; MAX_LEN=8192 ;;
    url)          SOURCES="std/net/aether_http.c"; MAX_LEN=4096 ;;
    json)         SOURCES="std/json/aether_json.c"; MAX_LEN=65536 ;;
    *)
        echo "usage: $0 http_head|http_chunked|url|json" >&2
        exit 2
        ;;
esac

TARGET_DIR="$SCRIPT_DIR/$TARGET"
HARNESS="$TARGET_DIR/fuzz_$TARGET.c"
CORPUS_DIR="$TARGET_DIR/corpus"
DICT_FILE="$TARGET_DIR/$TARGET.dict"
FUZZ_SECONDS="${FUZZ_SECONDS:-60}"

EXE=""
case "$(uname -s 2>/dev/null)" in MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;; esac
AE="$ROOT/build/ae$EXE"
if [ ! -x "$AE" ]; then
    echo "Error: $AE not built. Run 'make' first." >&2
    exit 1
fi

if [ -z "${FUZZ_STD_CFLAGS:-}" ]; then
    FUZZ_STD_CFLAGS="$(make -s --no-print-directory print-fuzz-std-cflags)"
fi
AE_CFLAGS="$("$AE" cflags)"

OUT="${FUZZ_OUT:-$ROOT/build/fuzz/$TARGET}"
mkdir -p "$OUT"

if [ "${FUZZ_REPLAY:-0}" = "1" ]; then
    CC="${CC:-cc}"
    echo "==> Building $OUT/replay_$TARGET$EXE with $CC (replay only, no libFuzzer)..."
    # shellcheck disable=SC2086
    "$CC" -g -O1 -o "$OUT/replay_$TARGET$EXE" \
        "$SCRIPT_DIR/replay_main.c" "$HARNESS" $SOURCES \
        $FUZZ_STD_CFLAGS $AE_CFLAGS
    echo "==> Replaying the seeds${FUZZ_REPLAY_INPUTS:+ and $FUZZ_REPLAY_INPUTS}..."
    # shellcheck disable=SC2086
    "$OUT/replay_$TARGET$EXE" "$CORPUS_DIR" ${FUZZ_REPLAY_INPUTS:-}
    exit $?
fi

CLANG="${CLANG:-clang}"
if ! command -v "$CLANG" >/dev/null 2>&1; then
    echo "Error: clang ($CLANG) not found." >&2
    exit 1
fi

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

echo "==> Building $OUT/fuzz_$TARGET..."
# The harness and the parsers under test carry the coverage and sanitizer
# instrumentation; they are linked ahead of libaether, whose copies of the
# same files are then never pulled in. UBSan halts rather than printing and
# carrying on, so undefined behaviour fails the run.
# shellcheck disable=SC2086
"$CLANG" -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -g -O1 \
    "$HARNESS" $SOURCES \
    $FUZZ_STD_CFLAGS $AE_CFLAGS \
    -o "$OUT/fuzz_$TARGET"

mkdir -p "$OUT/corpus"
cp "$CORPUS_DIR"/* "$OUT/corpus/"

# Fewer allocation frames keep ASan's trace store flat on a long run, as for
# the CBOR fuzzer. A caller's ASAN_OPTIONS wins.
export ASAN_OPTIONS="${ASAN_OPTIONS:-malloc_context_size=8}"

echo "==> Fuzzing $TARGET for ${FUZZ_SECONDS}s..."
set +e
"$OUT/fuzz_$TARGET" \
    -dict="$DICT_FILE" \
    -max_len="$MAX_LEN" \
    -rss_limit_mb=2048 \
    -timeout=10 \
    -max_total_time="$FUZZ_SECONDS" \
    -artifact_prefix="$OUT/" \
    -print_final_stats=1 \
    "$OUT/corpus"
EXIT_CODE=$?
set -e

if [ $EXIT_CODE -ne 0 ]; then
    echo "FAIL: fuzz_$TARGET exited $EXIT_CODE" >&2
    for repro in "$OUT"/crash-* "$OUT"/leak-* "$OUT"/timeout-* "$OUT"/oom-*; do
        [ -f "$repro" ] && echo "  reproducer: $repro  (rerun: $OUT/fuzz_$TARGET $repro)" >&2
    done
    exit $EXIT_CODE
fi

echo "==> No crash, leak, timeout or property failure in ${FUZZ_SECONDS}s."
