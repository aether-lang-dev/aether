#!/bin/sh
# Regression: fs.create_dir_with_mode honours the requested POSIX mode
# at creation, closing the previous mkdir-then-chmod race window. Users
# who need a private directory (e.g. 0700 for keys) can now get it
# without ever exposing 0755-default contents to a watcher.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] fs_create_dir_mode: $AE not built"
    exit 0
fi
case "$(uname -s)" in
    Linux|Darwin) : ;;
    *) echo "  [SKIP] fs_create_dir_mode: POSIX-only (Windows mkdir ignores mode)"; exit 0 ;;
esac

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

target="$tmpdir/private_keys"
cat > "$tmpdir/mkdir_priv.ae" <<EOF
import std.fs

main() {
    // 448 == 0o700
    ok, _kind, err = fs.create_dir_with_mode("$target", 448)
    if ok != 1 {
        println("create_dir_with_mode failed: \${err}")
        return
    }
    println("create_dir_with_mode ok")
}
EOF

if ! "$AE" build "$tmpdir/mkdir_priv.ae" -o "$tmpdir/mkp" >"$tmpdir/build.log" 2>&1; then
    echo "  [FAIL] fs_create_dir_mode: build failed"
    cat "$tmpdir/build.log"
    exit 1
fi
"$tmpdir/mkp" >"$tmpdir/run.log" 2>&1

if [ ! -d "$target" ]; then
    echo "  [FAIL] fs_create_dir_mode: directory not created"
    cat "$tmpdir/run.log"
    exit 1
fi

# Read the actual mode. `stat -c` (Linux GNU) vs `stat -f` (macOS BSD).
case "$(uname -s)" in
    Linux)  mode=$(stat -c '%a' "$target") ;;
    Darwin) mode=$(stat -f '%Lp' "$target") ;;
esac

# umask trims at most write-bits-others, so 0700 stays 0700 under any
# reasonable umask. Mode `700` is the canonical answer.
if [ "$mode" != "700" ]; then
    echo "  [FAIL] fs_create_dir_mode: expected mode 700, got $mode"
    exit 1
fi

echo "  [PASS] fs_create_dir_mode: directory created at requested 0700 mode"
exit 0
