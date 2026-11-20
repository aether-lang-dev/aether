#!/bin/sh
set -e

# Build and run one of the fuzz targets that share this runner:
#
#   http_head        the HTTP/1 request head on the server's path
#   http_chunked     the chunked body framing and decoder
#   url              the client's URL and Location parsers
#   json             the JSON parser and serializer
#   websocket        the WebSocket frame reader
#   h2               the HTTP/2 server glue over nghttp2
#   proxy_response   the reverse proxy's handling of an upstream response
#   xml              std.xml's reader, builder and pretty printer
#   yaml             std.yaml and the vendored libfyaml
#   msgpack          std.msgpack's unpack and pack (Aether)
#   tar              std.tar's reader and extraction (Aether)
#   zip              std.zip's reader and extraction (Aether)
#   tls13_record     the TLS 1.3 record layer (Aether)
#   tls13_handshake  the TLS 1.3 handshake message parsers (Aether)
#   x509             X.509 certificates, ASN.1 DER and PEM (Aether)
#
#   sh tests/fuzz/run_fuzz_c.sh <target>
#
# Each lives in tests/fuzz/<target>/. A C target is a harness,
# fuzz_<target>.c, built with the std sources it tests:
#   clang -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -g -O1
# so libFuzzer steers by the parsers' own branches and the sanitizers watch
# them, the rest of the runtime linked from build/libaether.a. An Aether
# target, fuzz_<target>.ae, exports fuzz_target(data, len); `ae build
# --emit=obj` compiles it with that clang, so the modules it imports carry the
# same instrumentation, and tests/fuzz/ae_target.c's LLVMFuzzerTestOneInput
# calls it. Either runs for FUZZ_SECONDS (default 60) on a copy of the seed
# corpus with the target's dictionary, and exits non-zero on a crash, leak,
# timeout or property failure, naming the reproducer.
#
# FUZZ_REPLAY=1 builds the same harness with $CC (default cc) and no
# libFuzzer, linked to tests/fuzz/replay_main.c, reads the target's dictionary
# as libFuzzer reads it (failing on a line libFuzzer would refuse) and runs
# each entry, then every seed once (plus any files or directories named in
# FUZZ_REPLAY_INPUTS). It works where clang has no libFuzzer, Windows among
# them, and finds nothing new.
#
# The Makefile's fuzz-<target> rules pass the flags the std sources are
# compiled with (FUZZ_STD_CFLAGS); run by hand, the script takes them from
# `make -s print-fuzz-std-cflags`. A target that writes files (tar, zip)
# writes them under $FUZZ_SCRATCH, build/fuzz/<target>/scratch.

TARGET="$1"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

# SOURCES: the C files compiled into a harness, with its instrumentation.
# AE_WITH: an Aether target's --with list. EXTRA: what else its build needs.
SOURCES=""; AE_WITH=""; EXTRA=""
case "$TARGET" in
    http_head)    SOURCES="std/net/aether_http.c std/net/aether_http_server.c"; MAX_LEN=8192 ;;
    http_chunked) SOURCES="std/net/aether_http.c"; MAX_LEN=8192 ;;
    url)          SOURCES="std/net/aether_http.c"; MAX_LEN=4096 ;;
    json)         SOURCES="std/json/aether_json.c"; MAX_LEN=65536 ;;
    # The harness includes aether_http_server.c itself, for the frame
    # reader's connection and handle types.
    websocket)    SOURCES="std/net/aether_http.c"; MAX_LEN=16384 ;;
    h2)           SOURCES="std/http/server/h2/aether_h2.c std/net/aether_http.c std/net/aether_http_server.c"
                  MAX_LEN=16384
                  if ! pkg-config --exists libnghttp2 2>/dev/null; then
                      echo "Error: the h2 target needs libnghttp2 (pkg-config libnghttp2): install libnghttp2-dev." >&2
                      exit 1
                  fi
                  EXTRA="-DAETHER_HAS_NGHTTP2 $(pkg-config --cflags --libs libnghttp2)" ;;
    # The harness includes aether_http.c itself, for the response framing
    # the proxy event loop applies (http_response_is_complete).
    proxy_response) SOURCES="std/http/proxy/aether_proxy_middleware.c std/net/aether_http_server.c"
                  MAX_LEN=16384 ;;
    xml)          SOURCES="std/xml/aether_xml.c"; MAX_LEN=16384 ;;
    yaml)         SOURCES="std/yaml/aether_yaml.c std/yaml/aether_libfyaml_vendored.c"; MAX_LEN=16384 ;;
    msgpack)      AE_WITH="extern"; MAX_LEN=65536 ;;
    tar)          AE_WITH="extern,fs,os"; MAX_LEN=32768 ;;
    zip)          AE_WITH="extern,fs,os"; MAX_LEN=32768 ;;
    tls13_record) AE_WITH="extern"; MAX_LEN=20000 ;;
    tls13_handshake) AE_WITH="extern"; MAX_LEN=16384 ;;
    x509)         AE_WITH="extern,os,fs"; MAX_LEN=16384 ;;
    *)
        echo "usage: $0 <target>: http_head, http_chunked, url, json, websocket, h2, proxy_response," >&2
        echo "       xml, yaml, msgpack, tar, zip, tls13_record, tls13_handshake or x509" >&2
        exit 2
        ;;
esac

TARGET_DIR="$SCRIPT_DIR/$TARGET"
CORPUS_DIR="$TARGET_DIR/corpus"
DICT_FILE="$TARGET_DIR/$TARGET.dict"
FUZZ_SECONDS="${FUZZ_SECONDS:-60}"
AE_SRC=""
if [ -n "$AE_WITH" ]; then
    AE_SRC="$TARGET_DIR/fuzz_$TARGET.ae"
    HARNESS="$SCRIPT_DIR/ae_target.c"
else
    HARNESS="$TARGET_DIR/fuzz_$TARGET.c"
fi

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
FUZZ_SCRATCH="$OUT/scratch"
export FUZZ_SCRATCH

# ae_object CC-COMMAND: compile an Aether target to $OUT/fuzz_<target>_ae.o
# with that C compiler command, so what it imports carries its flags.
AE_OBJ=""
ae_object() {
    echo "==> Building $OUT/fuzz_${TARGET}_ae.o..."
    AE_CC="$1" "$AE" build "$AE_SRC" --with="$AE_WITH" --emit=obj -o "$OUT/fuzz_${TARGET}_ae.o"
    AE_OBJ="$OUT/fuzz_${TARGET}_ae.o"
}

if [ "${FUZZ_REPLAY:-0}" = "1" ]; then
    CC="${CC:-cc}"
    [ -z "$AE_SRC" ] || ae_object "$CC -g -O1"
    echo "==> Building $OUT/replay_$TARGET$EXE with $CC (replay only, no libFuzzer)..."
    # shellcheck disable=SC2086
    "$CC" -g -O1 -o "$OUT/replay_$TARGET$EXE" \
        "$SCRIPT_DIR/replay_main.c" "$HARNESS" $SOURCES $AE_OBJ \
        $FUZZ_STD_CFLAGS $EXTRA $AE_CFLAGS
    echo "==> Replaying the dictionary and the seeds${FUZZ_REPLAY_INPUTS:+ and $FUZZ_REPLAY_INPUTS}..."
    # -dict reads the dictionary as libFuzzer does and fails on a line it
    # would refuse, so a bad entry fails here and not only on CI.
    # shellcheck disable=SC2086
    "$OUT/replay_$TARGET$EXE" -dict="$DICT_FILE" "$CORPUS_DIR" ${FUZZ_REPLAY_INPUTS:-}
    exit $?
fi

CLANG="${CLANG:-clang}"
if ! command -v "$CLANG" >/dev/null 2>&1; then
    echo "Error: clang ($CLANG) not found." >&2
    exit 1
fi

CHECK_TMP="$(mktemp -d)"
check_supported=1
printf '#include <stdint.h>\n#include <stddef.h>\nint LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) { (void)d; (void)n; return 0; }\n' \
    > "$CHECK_TMP/check_fuzzer.c"
if ! "$CLANG" -fsanitize=fuzzer "$CHECK_TMP/check_fuzzer.c" -o "$CHECK_TMP/check_fuzzer" >/dev/null 2>&1; then
    check_supported=0
fi
rm -rf "$CHECK_TMP"
if [ "$check_supported" -eq 0 ]; then
    echo "Error: compiler ($CLANG) lacks libFuzzer support (-fsanitize=fuzzer failed to link)." >&2
    echo "Install libclang-rt-<version>-dev (e.g. libclang-rt-18-dev on Ubuntu) or use a clang build with compiler-rt built-in." >&2
    exit 1
fi

# UBSan halts rather than printing and carrying on, so undefined behaviour
# fails the run.
[ -z "$AE_SRC" ] || ae_object "$CLANG -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined -g -O1"

echo "==> Building $OUT/fuzz_$TARGET..."
# The harness and the parsers under test carry the coverage and sanitizer
# instrumentation; they are linked ahead of libaether, whose copies of the
# same files are then never pulled in.
# shellcheck disable=SC2086
"$CLANG" -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -g -O1 \
    "$HARNESS" $SOURCES $AE_OBJ \
    $FUZZ_STD_CFLAGS $EXTRA $AE_CFLAGS \
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
