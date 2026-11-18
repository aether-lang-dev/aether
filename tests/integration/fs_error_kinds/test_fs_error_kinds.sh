#!/bin/sh
# std.fs reports EFBIG and EROFS as their own kinds, KIND_FILE_TOO_BIG and
# KIND_READ_ONLY_FS, rather than the catch-all KIND_IO, which left callers
# reading fs.last_os_error() and comparing POSIX numbers that are wrong on
# Windows. Each case makes the real condition:
#
#   EFBIG  a file-size limit (ulimit -f, SIGXFSZ ignored) and fd_truncate
#          past it. Every POSIX host.
#   EROFS  a read-only volume: a read-only disk image on macOS, a read-only
#          tmpfs in a user namespace on Linux (skipped where unprivileged
#          user namespaces are off). create_dir_with_mode and unlink in it.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=fs_error_kinds

if [ ! -x "$AE" ]; then
    echo "  [SKIP] $NAME: $AE not built"
    exit 0
fi
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) echo "  [SKIP] $NAME: POSIX limits and mounts"; exit 0 ;;
esac

tmpdir="$(mktemp -d)"
mnt="$tmpdir/mnt"
cleanup() {
    if [ "$(uname -s)" = Darwin ] && [ -d "$mnt" ]; then
        hdiutil detach -quiet "$mnt" >/dev/null 2>&1
    fi
    rm -rf "$tmpdir"
}
trap cleanup EXIT

fail() {
    echo "  [FAIL] $NAME: $1"
    [ -f "$2" ] && cat "$2"
    exit 1
}

# ---- EFBIG ----
cat > "$tmpdir/efbig.ae" <<EOF
import std.fs

main() {
    fd, k, m = fs.create_exclusive("$tmpdir/big.bin", 384)
    if k != fs.KIND_OK {
        println("create_exclusive failed: \${m}")
        return
    }
    ok, tk, _tm = fs.fd_truncate(fd, 1048576)
    println("truncate \${ok} \${tk}")
}
EOF
"$AE" build "$tmpdir/efbig.ae" -o "$tmpdir/efbig" >"$tmpdir/build.log" 2>&1 \
    || fail "efbig.ae did not build" "$tmpdir/build.log"
# 8 blocks is 4 KB under sh's 512-byte blocks and 8 KB under bash's 1 KB.
sh -c "trap '' XFSZ; ulimit -f 8 && exec '$tmpdir/efbig'" >"$tmpdir/efbig.log" 2>&1
got="$(cat "$tmpdir/efbig.log")"
[ "$got" = "truncate 0 14" ] \
    || fail "fd_truncate past ulimit -f: expected 'truncate 0 14', got '$got'"
echo "  [PASS] $NAME: EFBIG is KIND_FILE_TOO_BIG"

# ---- EROFS ----
cat > "$tmpdir/erofs.ae" <<EOF
import std.fs

main() {
    ok, k, m = fs.create_dir_with_mode("$mnt/made", 448)
    println("mkdir \${ok} \${k} \${m}")
    when target.os == "darwin" {
        uok, uk, um = fs.unlink("$mnt/present.txt")
        println("unlink \${uok} \${uk} \${um}")
    }
}
EOF
"$AE" build "$tmpdir/erofs.ae" -o "$tmpdir/erofs" >"$tmpdir/build.log" 2>&1 \
    || fail "erofs.ae did not build" "$tmpdir/build.log"

case "$(uname -s)" in
    Darwin)
        mkdir -p "$tmpdir/src" "$mnt"
        echo x > "$tmpdir/src/present.txt"
        hdiutil create -quiet -srcfolder "$tmpdir/src" -format UDRO -fs HFS+ \
            "$tmpdir/ro.dmg" >"$tmpdir/hdi.log" 2>&1 \
            || fail "hdiutil create failed" "$tmpdir/hdi.log"
        hdiutil attach -quiet -readonly -nobrowse -mountpoint "$mnt" "$tmpdir/ro.dmg" \
            >"$tmpdir/hdi.log" 2>&1 || fail "hdiutil attach failed" "$tmpdir/hdi.log"
        "$tmpdir/erofs" >"$tmpdir/erofs.log" 2>&1
        want="mkdir 0 12 read-only file system
unlink 0 12 read-only file system"
        ;;
    Linux)
        mkdir -p "$mnt"
        if ! unshare -rm sh -c "mount -t tmpfs -o ro none '$mnt'" >/dev/null 2>&1; then
            echo "  [SKIP] $NAME: EROFS (no unprivileged user namespaces here)"
            exit 0
        fi
        unshare -rm sh -c "mount -t tmpfs -o ro none '$mnt' && exec '$tmpdir/erofs'" \
            >"$tmpdir/erofs.log" 2>&1
        want="mkdir 0 12 read-only file system"
        ;;
    *)
        echo "  [SKIP] $NAME: EROFS (needs a read-only mount this host can make unprivileged)"
        exit 0
        ;;
esac
got="$(cat "$tmpdir/erofs.log")"
[ "$got" = "$want" ] || fail "read-only volume: expected '$want', got '$got'"
echo "  [PASS] $NAME: EROFS is KIND_READ_ONLY_FS"
exit 0
