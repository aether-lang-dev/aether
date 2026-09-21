#!/bin/sh
# `fs.open` hands back a binary handle on every platform.
#
# Every other std.fs entry point opens "rb"/"wb" (fs.read, fs.write,
# read_binary, write_binary, write_atomic) and std.io opens O_BINARY, but
# fs.open passed the caller's mode to fopen as written, so on Windows
# `fs.open(p, "w")` was a text stream: a '\n' written through pwrite became
# "\r\n" on disk (the http_stream_upload server streamed 3 MiB to a file and
# hashed 8 bytes for every 6), and a "r" handle folded "\r\n" back to "\n"
# and stopped at the first 0x1A. pread/pwrite are documented binary-safe.
# The 'b' is a no-op on POSIX, so this passes everywhere and fails only
# where it used to be wrong.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] fs_open_binary_mode: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT

cat > "$tmp/main.ae" <<'AE'
import std.fs
import std.os
import std.string
extern file_close(file: ptr) -> int
extern file_read_all_raw(file: ptr) -> string

main() {
    dir = aether_args_get(1)
    p = "${dir}/lf.bin"
    // 1. "w" + pwrite: a '\n' is one byte on disk
    f, oerr = fs.open(p, "w")
    if oerr != "" { println("open w: ${oerr}"); return 1 }
    n, werr = fs.pwrite(f, "ab\ncd\n", 6, 0)
    _ = file_close(f)
    sz, serr = fs.size(p)
    println("pwrite ${n} ${werr} size ${sz}")
    // 2. "r" + pread reads the bytes back exactly
    g, gerr = fs.open(p, "r")
    if gerr != "" { println("open r: ${gerr}"); return 1 }
    got, gn, perr = fs.pread(g, 16, 0)
    _ = file_close(g)
    println("pread ${gn} ${got == "ab\ncd\n"}")
    // 3. a file holding CRLF and a 0x1A byte is read verbatim through "r"
    q = "${dir}/crlf.bin"
    raw = "x\r\ny\r\n"
    _ = fs.write_binary(q, raw, 6)
    h, herr = fs.open(q, "r")
    if herr != "" { println("open r2: ${herr}"); return 1 }
    back = file_read_all_raw(h)
    _ = file_close(h)
    println("crlf ${string.length(back)} ${back == raw}")
    // 4. "a+" gets the 'b' after the '+' and still appends
    k, kerr = fs.open(p, "a+")
    if kerr != "" { println("open a+: ${kerr}"); return 1 }
    _, aerr = fs.pwrite(k, "e\n", 2, 6)
    _ = file_close(k)
    sz2, _ = fs.size(p)
    println("append ${aerr} size ${sz2}")
    // 5. an explicit 't' is passed through, not turned into "wtb" (which
    //    UCRT rejects): the open succeeds
    w, terr = fs.open("${dir}/text.txt", "wt")
    println("wt ${w != null} ${terr}")
    if w != null { _ = file_close(w) }
}
AE
want='pwrite 6  size 6
pread 6 true
crlf 6 true
append  size 8
wt true '
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/main.ae" -- "$tmp" 2>&1 | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] fs_open_binary_mode: output differs"
    printf '%s\n' "$got" | head -8 | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] fs_open_binary_mode: fs.open handles are byte-exact through pwrite/pread and read_all; a+ appends"
exit 0
