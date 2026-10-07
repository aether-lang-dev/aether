#!/bin/sh
# On Windows, a directory junction (`mklink /J`, no privilege needed) counts
# as a link for fs_is_symlink, and readlink returns its target.
#
# fs_is_symlink accepted only IO_REPARSE_TAG_SYMLINK, so a junction looked
# like an ordinary directory. Archive extraction checks each parent of an
# entry with it, and wrote through a junction under the destination to a
# directory outside it. A junction redirects path resolution exactly as a
# symlink does; both are name-surrogate reparse points.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) ;;
    *) echo "  [SKIP] fs_windows_junction_is_link: junctions are a Windows feature (POSIX symlinks are covered by std.fs's tests)"; exit 0 ;;
esac
if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] fs_windows_junction_is_link: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
cleanup() {
    # Remove the junction itself first (rmdir on a junction removes the link,
    # not the target's contents).
    [ -d "$tmp/link" ] && cmd //c rmdir "$(cygpath -w "$tmp/link")" >/dev/null 2>&1
    rm -rf "$tmp" || true
}
trap cleanup EXIT
export AETHER_HOME="$ROOT"

mkdir -p "$tmp/target" "$tmp/tree"
echo keep > "$tmp/target/keep.txt"
if ! cmd //c mklink //J "$(cygpath -w "$tmp/link")" "$(cygpath -w "$tmp/target")" >/dev/null 2>&1; then
    echo "  [SKIP] fs_windows_junction_is_link: mklink /J failed here"
    exit 0
fi
# A junction inside a tree that remove_tree deletes: the junction goes, the
# directory it points at stays. remove_tree followed it and emptied target.
cmd //c mklink //J "$(cygpath -w "$tmp/tree/inner")" "$(cygpath -w "$tmp/target")" >/dev/null 2>&1

cat > "$tmp/main.ae" <<'AE'
import std.fs
import std.os
import std.string

main() {
    base = os.os_getenv("JT_DIR")
    println("link ${fs.fs_is_symlink("${base}/link")}")
    println("dir ${fs.fs_is_symlink("${base}/target")}")
    t = fs.fs_readlink_raw("${base}/link")
    println("target-ends ${string.string_ends_with(t, "target")}")
    rerr = fs.remove_tree("${base}/tree")
    println("remove_tree [${rerr}] tree-gone ${fs.fs_path_exists("${base}/tree") == 0} keep-kept ${fs.fs_path_exists("${base}/target/keep.txt")}")
}
AE

got="$(JT_DIR="$(cygpath -m "$tmp")" "$AE" run "$tmp/main.ae" 2>&1 | tr -d '\r' | grep -E '^(link|dir|target-ends|remove_tree) ')"
want="link 1
dir 0
target-ends 1
remove_tree [] tree-gone true keep-kept 1"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] fs_windows_junction_is_link"
    printf 'got:\n%s\nwant:\n%s\n' "$got" "$want" | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] fs_windows_junction_is_link: a junction is a link to fs_is_symlink and readlink, and remove_tree does not follow it"
