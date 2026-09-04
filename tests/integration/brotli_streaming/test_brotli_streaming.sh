#!/bin/sh
# #1891: brotli streaming holds ONE stream open across many flushes.
#
# `br` is what browsers actually prefer over gzip, so this is the higher-value
# codec for HTTP. The surface deliberately mirrors std.zlib's streaming
# deflate, so a server negotiating br vs gzip writes the same shape either way.
#
# Two assertions, as with the zlib test:
#   1. every flushed chunk concatenated is ONE stream, and later events cost
#      far less than the first -- proving a shared window rather than N
#      independent streams (prog.ae);
#   2. an INDEPENDENT decoder accepts the bytes. We only have an encoder, so
#      this uses the system libbrotlidec directly: our encoder agreeing with
#      itself would prove nothing about RFC 7932 framing.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] brotli_streaming: ae not built"; exit 0; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

OUT=$("$AE" run "$SCRIPT_DIR/prog.ae" 2>&1) || {
    echo "  [FAIL] brotli_streaming: did not build/run"
    echo "$OUT" | sed 's/^/    /' | head -12
    exit 1
}
case "$OUT" in
    *"skipped: no backend"*)
        echo "  [SKIP] brotli_streaming: no libbrotlienc"; exit 0 ;;
    *"brotli streaming ok"*) ;;
    *)
        echo "  [FAIL] brotli_streaming: assertions did not pass"
        echo "$OUT" | sed 's/^/    /' | head -12
        exit 1 ;;
esac

# --- 2. an independent decoder must accept the framing ------------------
if ! pkg-config --exists libbrotlidec 2>/dev/null; then
    echo "  [PASS] brotli_streaming: one stream across flushes (no libbrotlidec, external check skipped)"
    exit 0
fi

# Heredoc UNQUOTED so $TMP expands; Aether ${...} escaped as \${...}.
# (Written rather than sed-patched: BSD `sed -i` needs a backup suffix and
# GNU sed does not, so a substitution here breaks on one platform or the
# other -- the same trap tests/integration/cache_symlinked_lib_edit notes.)
cat > "$TMP/emit.ae" <<AEOF
import std.brotli
import std.string
import std.fs

main() {
    if brotli.available() == 0 { return 0 }
    s, err = brotli.stream_new(5, 0)
    if err != "" { println("new: \${err}") return 1 }
    all = ""
    i = 0
    while i < 3 {
        ev = "event: patch\ndata: chunk-\${i}\n\n"
        c1, n1, e1 = brotli.stream_write(s, ev, string.length(ev))
        if n1 > 0 { all = string.concat(all, c1) }
        c2, n2, e2 = brotli.stream_flush(s)
        if n2 > 0 { all = string.concat(all, c2) }
        i = i + 1
    }
    t, tn, e3 = brotli.stream_finish(s)
    if tn > 0 { all = string.concat(all, t) }
    brotli.stream_free(s)
    werr = fs.write_binary("$TMP/out.br", all, string.length(all))
    if werr != "" { println("write: \${werr}") return 1 }
    return 0
}
AEOF

"$AE" run "$TMP/emit.ae" >"$TMP/emit.log" 2>&1 || {
    echo "  [FAIL] brotli_streaming: fixture did not run"
    sed 's/^/    /' "$TMP/emit.log" | head -8; exit 1; }
[ -f "$TMP/out.br" ] || { echo "  [FAIL] brotli_streaming: no .br written"; exit 1; }

cat > "$TMP/dec.c" <<'CEOF'
#include <stdio.h>
#include <brotli/decode.h>
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    FILE* f = fopen(argv[1], "rb"); if (!f) return 2;
    static unsigned char in[1<<20]; size_t n = fread(in, 1, sizeof(in), f); fclose(f);
    static unsigned char out[1<<20]; size_t out_n = sizeof(out);
    if (BrotliDecoderDecompress(n, in, &out_n, out) != BROTLI_DECODER_RESULT_SUCCESS) {
        fprintf(stderr, "decode failed\n"); return 1;
    }
    fwrite(out, 1, out_n, stdout);
    return 0;
}
CEOF
CC_BIN="${CC:-cc}"
if ! "$CC_BIN" -o "$TMP/dec" "$TMP/dec.c" $(pkg-config --cflags --libs libbrotlidec) >"$TMP/cc.log" 2>&1; then
    echo "  [PASS] brotli_streaming: one stream across flushes (decoder probe would not build, external check skipped)"
    exit 0
fi

GOT=$("$TMP/dec" "$TMP/out.br" 2>"$TMP/dec.err") || {
    echo "  [FAIL] brotli_streaming: an independent brotli decoder REJECTED the stream"
    sed 's/^/    /' "$TMP/dec.err" | head -5
    exit 1; }

for n in 0 1 2; do
    case "$GOT" in
        *"chunk-$n"*) ;;
        *) echo "  [FAIL] brotli_streaming: decoder output missing chunk-$n"; exit 1 ;;
    esac
done

echo "  [PASS] brotli_streaming: one stream across flushes, and libbrotlidec reads it"
