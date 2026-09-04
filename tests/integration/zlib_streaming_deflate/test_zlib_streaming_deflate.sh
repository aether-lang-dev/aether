#!/bin/sh
# #1890: streaming deflate holds ONE stream open across many flushes.
#
# The one-shot calls each emit a complete stream. A long-lived response (SSE,
# a chunked body) needs one stream flushed at each event boundary, or the
# client gets N concatenated streams and refuses them.
#
# Two assertions, because the in-process one alone is not enough:
#   1. every flushed chunk, concatenated, decodes as ONE stream (prog.ae),
#      and later flushes cost far less than the first -- proving the window
#      is shared rather than N independent streams;
#   2. real `gunzip` accepts the bytes. Our own inflate agreeing with our own
#      deflate would prove very little; the point of RFC 1952 framing is that
#      a foreign decoder reads it.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] zlib_streaming_deflate: ae not built"; exit 0; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

OUT=$("$AE" run "$SCRIPT_DIR/prog.ae" 2>&1) || {
    echo "  [FAIL] zlib_streaming_deflate: did not build/run"
    echo "$OUT" | sed 's/^/    /' | head -12
    exit 1
}
case "$OUT" in
    *"skipped: no backend"*)
        echo "  [SKIP] zlib_streaming_deflate: no zlib backend"; exit 0 ;;
    *"zlib streaming ok"*) ;;
    *)
        echo "  [FAIL] zlib_streaming_deflate: assertions did not pass"
        echo "$OUT" | sed 's/^/    /' | head -12
        exit 1 ;;
esac

# --- 2. a FOREIGN decoder must accept the framing -----------------------
if ! command -v gunzip >/dev/null 2>&1; then
    echo "  [PASS] zlib_streaming_deflate: one stream across flushes (gunzip absent, external check skipped)"
    exit 0
fi

cat > "$TMP/emit.ae" <<'AEOF'
import std.zlib
import std.string
import std.fs

main() {
    if zlib.available() == 0 { return 0 }
    s, err = zlib.stream_new(zlib.GZIP, 6)
    if err != "" { println("new: ${err}") return 1 }
    all = ""
    i = 0
    while i < 3 {
        ev = "event: patch\ndata: chunk-${i}\n\n"
        c1, n1, e1 = zlib.stream_write(s, ev, string.length(ev))
        if n1 > 0 { all = string.concat(all, c1) }
        c2, n2, e2 = zlib.stream_flush(s)
        if n2 > 0 { all = string.concat(all, c2) }
        i = i + 1
    }
    t, tn, e3 = zlib.stream_finish(s)
    if tn > 0 { all = string.concat(all, t) }
    zlib.stream_free(s)
    werr = fs.write_binary(OUT_PATH, all, string.length(all))
    if werr != "" { println("write: ${werr}") return 1 }
    return 0
}
AEOF
# Substitute the output path (no argv plumbing needed for a fixture).
sed -i "s|OUT_PATH|\"$TMP/out.gz\"|" "$TMP/emit.ae"

"$AE" run "$TMP/emit.ae" >"$TMP/emit.log" 2>&1 || {
    echo "  [FAIL] zlib_streaming_deflate: fixture did not run"
    sed 's/^/    /' "$TMP/emit.log" | head -8; exit 1; }
[ -f "$TMP/out.gz" ] || { echo "  [FAIL] zlib_streaming_deflate: no .gz written"; exit 1; }

GOT=$(gunzip -c "$TMP/out.gz" 2>"$TMP/gz.err") || {
    echo "  [FAIL] zlib_streaming_deflate: real gunzip REJECTED the stream"
    sed 's/^/    /' "$TMP/gz.err" | head -5
    exit 1; }

# All three events must be there, in order.
for n in 0 1 2; do
    case "$GOT" in
        *"chunk-$n"*) ;;
        *) echo "  [FAIL] zlib_streaming_deflate: gunzip output missing chunk-$n"; exit 1 ;;
    esac
done

echo "  [PASS] zlib_streaming_deflate: one stream across flushes, and real gunzip reads it"
