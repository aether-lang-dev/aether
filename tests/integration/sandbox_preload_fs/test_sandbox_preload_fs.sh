#!/bin/sh
# Under the LD_PRELOAD library, the file-system calls it did not wrap before
# #3751 are checked, the *at calls resolve against their directory
# descriptor, and a call is made on the path its check resolved.
#
#   - openat(dirfd, "../outside/x") from a descriptor of the box is outside:
#     it was resolved against the working directory (box/sub here, where
#     "../outside" is box/outside), passed, and opened the outside file.
#   - unlink, remove, rename, mkdir, rmdir, symlink, link, chmod, chown,
#     truncate, utimes, mkfifo, creat, __open_2, opendir, scandir, mkstemp,
#     mkdtemp and posix_spawn_file_actions_addopen were not wrapped at all.
#   - a hard link may not put an outside file under the box's grants.
#   - glob and nftw do not hand back what lies behind a symlink out.
#   - a directory swapped for a symlink between the check and the open is
#     not followed (the race case: a thread swaps them 20000 times).
#
# The grants cover the box (read, list) and what is in it (read, write);
# nothing outside.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LIB="$ROOT/build/libaether_sandbox.so"

os="$(uname -s 2>/dev/null)"
case "$os" in
    Linux|FreeBSD) ;;
    *)
        echo "  [SKIP] sandbox_preload_fs: preload is Linux and FreeBSD only"
        exit 0 ;;
esac
if [ ! -f "$LIB" ]; then
    echo "  [SKIP] sandbox_preload_fs: preload library not built"
    exit 0
fi

TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT
if ! ${CC:-cc} -std=c11 -Wall -Wextra -Werror -pthread \
        "$SCRIPT_DIR/harness.c" -o "$TMP/harness"; then
    echo "  [FAIL] sandbox_preload_fs: harness did not compile"
    exit 1
fi

FIX="$TMP/root"
mkdir -p "$FIX/box/sub" "$FIX/box/d" "$FIX/outside/empty"
printf 'inside\n' > "$FIX/box/inside.txt"
printf 'inside\n' > "$FIX/box/d/secret.txt"
printf 'del\n' > "$FIX/box/del.txt"
printf 'ren\n' > "$FIX/box/ren.txt"
printf 'outside\n' > "$FIX/outside/secret.txt"
ln -s "$FIX/outside" "$FIX/box/d_link"
ln -s "$FIX/outside" "$FIX/box/out_dir"
ln -s "$FIX/outside/secret.txt" "$FIX/box/out_link"
ln -s "$FIX/box/inside.txt" "$FIX/outside/in_link"
printf 'fs_read:%s\nfs_read:%s/*\nfs_write:%s/*\n' \
    "$FIX/box" "$FIX/box" "$FIX/box" > "$TMP/box.grants"
fail=0

# expected: 0 done, 3 refused (EACCES), 5 escaped, 6 EEXIST, 7 not here.
run_case() {
    label="$1"
    case_name="$2"
    expected="$3"
    also="${4:-}"
    (cd "$FIX/box/sub" && LD_PRELOAD="$LIB" AETHER_SANDBOX_GRANTS="$TMP/box.grants" \
        AETHER_SANDBOX_LOG=none "$TMP/harness" "$case_name" "$FIX" \
        </dev/null >"$TMP/out" 2>"$TMP/err")
    got=$?
    if [ "$got" -eq 7 ] && [ "$expected" -ne 7 ]; then
        echo "  [SKIP] $label: not on this system"
    elif [ "$got" -eq "$expected" ] || { [ -n "$also" ] && [ "$got" -eq "$also" ]; }; then
        echo "  [PASS] $label"
    else
        echo "  [FAIL] $label: exit $got, expected $expected"
        sed 's/^/    /' "$TMP/err"
        fail=1
    fi
}

run_case "openat from a box descriptor to ../outside is refused" openat-dirfd-out 3
run_case "openat from a box descriptor to a box file opens"      openat-dirfd-in  0
run_case "__open_2 (fortified open) outside is refused"          open2-out        3
run_case "creat outside is refused"                              creat-out        3
run_case "opendir outside is refused"                            opendir-out      3
run_case "scandir outside is refused"                            scandir-out      3
run_case "scandir of the box lists it"                           scandir-in       0
run_case "glob leaves out a match through a symlink out"         glob-filter      0
run_case "nftw passes nothing behind a symlink out"              nftw-filter      0
run_case "a directory swapped for a symlink is not followed"     race             0
run_case "unlink outside is refused"                             unlink-out       3
run_case "unlinkat from a box descriptor to ../outside is refused" unlinkat-out   3
run_case "unlink in the box removes"                             unlink-in        0
run_case "unlink of a symlink outside pointing in is refused"    unlink-link-out  3
run_case "unlink of a symlink in the box pointing out removes it" unlink-link-in  0
run_case "remove outside is refused"                             remove-out       3
run_case "rename out of outside is refused"                      rename-out       3
run_case "rename in the box renames"                             rename-in        0
run_case "mkdir outside is refused"                              mkdir-out        3
run_case "mkdir in the box makes"                                mkdir-in         0
run_case "mkdir of an existing directory says EEXIST"            mkdir-exists     6
run_case "rmdir outside is refused"                              rmdir-out        3
run_case "symlink outside is refused"                            symlink-out      3
run_case "a hard link of an outside file into the box is refused" link-widen      3
run_case "a hard link within the box links"                      link-in          0
run_case "chmod outside is refused"                              chmod-out        3
run_case "chmod in the box changes"                              chmod-in         0
run_case "chown outside is refused"                              chown-out        3
run_case "truncate outside is refused"                           truncate-out     3
run_case "utimes outside is refused"                             utimes-out       3
run_case "mkfifo outside is refused"                             mkfifo-out       3
run_case "mkstemp outside is refused"                            mkstemp-out      3
run_case "mkstemp in the box makes"                              mkstemp-in       0
run_case "mkdtemp outside is refused"                            mkdtemp-out      3
run_case "posix_spawn's addopen outside is refused"              spawn-addopen-out 3

if [ "$(cat "$FIX/outside/secret.txt" 2>/dev/null)" != "outside" ]; then
    echo "  [FAIL] the outside file was changed or removed"
    fail=1
fi
for made in created newdir new_link fifo; do
    if [ -e "$FIX/outside/$made" ] || [ -L "$FIX/outside/$made" ]; then
        echo "  [FAIL] outside/$made was made by a call that was refused"
        fail=1
    fi
done
if [ -e "$FIX/box/stolen" ] || [ -e "$FIX/box/hard" ]; then
    echo "  [FAIL] an outside file was moved or linked into the box"
    fail=1
fi
if [ ! -d "$FIX/outside/empty" ] || [ ! -L "$FIX/outside/in_link" ]; then
    echo "  [FAIL] a refused rmdir or unlink removed something outside"
    fail=1
fi

echo ""
echo "sandbox_preload_fs: $([ "$fail" -eq 0 ] && echo PASS || echo FAIL)"
[ "$fail" -eq 0 ]
