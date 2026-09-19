#!/bin/sh
# The language server answers the protocol: `initialize` gets its
# capabilities back, a `textDocument/didOpen` of a broken file gets a
# `publishDiagnostics` notification, `shutdown` gets `null`, and every
# message is framed byte-exactly (`Content-Length: N\r\n\r\n` + N bytes).
#
# The server's own method extraction used to find the opening quote of the
# KEY ("method") and take what followed its closing quote, so every request
# arrived as method ":" and nothing was ever answered — an editor waited on
# `initialize` forever. The only test the server had checked that it started
# and exited. On Windows the framing also went through the CRT's text mode,
# which turned the header terminator into \r\r\n\r\r\n.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LSP="$ROOT/build/aether-lsp"
[ -x "$LSP" ] || LSP="$ROOT/build/aether-lsp.exe"
if [ ! -x "$LSP" ]; then
    echo "  [SKIP] lsp_protocol_roundtrip: aether-lsp not built (make lsp)"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0

# One framed message per call; bodies are single-line JSON.
frame() {
    printf 'Content-Length: %d\r\n\r\n%s' "$(printf '%s' "$1" | wc -c | tr -d ' ')" "$1"
}
{
    frame '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{}}}'
    frame '{"jsonrpc":"2.0","method":"initialized","params":{}}'
    frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///broken.ae","languageId":"aether","version":1,"text":"main() {\n    x = \n}\n"}}}'
    frame '{"jsonrpc":"2.0","id":2,"method":"shutdown","params":null}'
    frame '{"jsonrpc":"2.0","method":"exit","params":null}'
} > "$tmp/in.bin"

AETHER_LSP_LOG="$tmp/lsp.log" "$LSP" < "$tmp/in.bin" > "$tmp/out.bin" 2> "$tmp/err.txt"
rc=$?
if [ $rc -ne 0 ]; then
    echo "  [FAIL] lsp_protocol_roundtrip: server exited with $rc"
    head -5 "$tmp/err.txt" | sed 's/^/        /'
    fail=1
fi

# Framing: the header terminator is exactly \r\n\r\n (two carriage returns
# per message, none doubled by a text-mode stream), and the first message's
# Content-Length is the byte length of its body. Bodies are single-line
# JSON, so the body is the third line once the \r are dropped.
headers="$(grep -c 'Content-Length: ' "$tmp/out.bin")"
crs="$(tr -cd '\r' < "$tmp/out.bin" | wc -c | tr -d ' ')"
if [ "$headers" -eq 0 ] || [ "$crs" -ne $((headers * 2)) ]; then
    echo "  [FAIL] lsp_protocol_roundtrip: $headers headers but $crs carriage returns (text-mode framing?)"
    fail=1
fi
# A body has no trailing newline, so the next message's header follows it
# on the same line: the body is the first Content-Length bytes of that
# line, and what remains must be the next header (or nothing) — which is
# what makes the declared length exact.
first_len="$(tr -d '\r' < "$tmp/out.bin" | sed -n '1s/^Content-Length: \([0-9]*\)$/\1/p')"
line3="$(tr -d '\r' < "$tmp/out.bin" | sed -n '3p')"
first_body="$(printf '%s' "$line3" | head -c "${first_len:-0}")"
rest="$(printf '%s' "$line3" | tail -c +"$((${first_len:-0} + 1))")"
case "$rest" in
    ""|Content-Length:*) ;;
    *) echo "  [FAIL] lsp_protocol_roundtrip: the first message's Content-Length ($first_len) does not match its body"
       printf '%s\n' "$rest" | head -c 80 | sed 's/^/        /'; echo
       fail=1 ;;
esac

if ! printf '%s' "$first_body" | grep -q '"id":1,"result":{"capabilities"'; then
    echo "  [FAIL] lsp_protocol_roundtrip: initialize was not answered with capabilities"
    printf '%s\n' "$first_body" | head -c 200 | sed 's/^/        /'; echo
    fail=1
fi
if ! grep -q 'textDocument/publishDiagnostics' "$tmp/out.bin"; then
    echo "  [FAIL] lsp_protocol_roundtrip: no publishDiagnostics for the broken document"
    fail=1
fi
if ! grep -q 'file:///broken.ae' "$tmp/out.bin"; then
    echo "  [FAIL] lsp_protocol_roundtrip: the diagnostics do not name the opened document"
    fail=1
fi
if ! grep -q '"id":2,"result":null' "$tmp/out.bin"; then
    echo "  [FAIL] lsp_protocol_roundtrip: shutdown was not answered with null"
    fail=1
fi
if ! grep -q 'Received: initialize (id: 1)' "$tmp/lsp.log"; then
    echo "  [FAIL] lsp_protocol_roundtrip: the log does not show initialize being received by name"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] lsp_protocol_roundtrip: initialize/didOpen/shutdown answered with exact framing"
fi
exit $fail
