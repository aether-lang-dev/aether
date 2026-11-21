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
mkdir -p "$FIX/box/sub" "$FIX/box/d" "$FIX/box/rmd" "$FIX/outside/empty"
printf 'ren\n' > "$FIX/box/ren_s.txt"
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

# expected: 0 done, 3 refused (EACCES), 4 another failure, 5 escaped,
# 6 EEXIST, 7 not here, 8 ENOTDIR, 9 EISDIR.
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
run_case "unlink of 'file/' is ENOTDIR and removes nothing"      unlink-slash-file 8
run_case "unlink of 'link/' is ENOTDIR and leaves the link"      unlink-slash-link 8
run_case "an open that would create 'new/' is EISDIR"            open-creat-slash  9
run_case "rmdir of 'dir/' removes the directory"                 rmdir-slash       0
run_case "a rename of a file to 'x/' is ENOTDIR"                 rename-slash-file 8
run_case "an O_TMPFILE file linked by /proc/self/fd/N gets its name" link-tmpfile-proc 0
# AT_EMPTY_PATH needs CAP_DAC_READ_SEARCH before Linux 6.10: the call is
# expected to do under the preload what it does without it.
(cd "$FIX/box/sub" && "$TMP/harness" link-tmpfile-empty "$FIX" </dev/null >/dev/null 2>&1)
free_empty=$?
rm -f "$FIX/box/tmp_empty"
run_case "an O_TMPFILE file linked by AT_EMPTY_PATH does what it does unsandboxed" link-tmpfile-empty "$free_empty"
(cd "$FIX/box/sub" && LD_PRELOAD="$LIB" AETHER_SANDBOX_GRANTS="$TMP/box.grants" \
    AETHER_SANDBOX_LOG=none "$TMP/harness" link-fd-out "$FIX" \
    </dev/null 9<"$FIX/outside/secret.txt" >"$TMP/out" 2>"$TMP/err")
got=$?
if [ "$got" -eq 3 ]; then
    echo "  [PASS] an inherited descriptor of an outside file gets no name in the box"
else
    echo "  [FAIL] link of an inherited outside descriptor into the box: exit $got, expected 3"
    fail=1
fi
run_case "a bind mount of a directory outside onto the box is refused" bind-mount-out 3
run_case "renameat2 out of outside is refused"                   renameat2-out    3
run_case "glob_b leaves out a match through a symlink out"       glob-b-filter    0
run_case "fts_open_b of outside is refused"                      fts-b-out        3
run_case "openat2 of outside is refused"                         openat2-out      3
run_case "openat2 with RESOLVE_NO_SYMLINKS of outside is refused" openat2-resolve-out 3
run_case "openat2 in the box opens"                              openat2-in       0
run_case "openat2 with RESOLVE_NO_SYMLINKS in the box opens"     openat2-resolve-in 0
# A refusal logged to the default log, aether-sandbox.log in the working
# directory, where the grants do not reach. The preload opens its log for
# itself; it opened it with libc's fopen, which on FreeBSD opens through
# _open, a call this library wraps: the log line of the refusal was itself
# refused, and logged, without end, until the stack ran out (a segfault).
# The refusal must also still read EACCES after the log line is written.
(cd "$FIX/outside" && LD_PRELOAD="$LIB" AETHER_SANDBOX_GRANTS="$TMP/box.grants" \
    "$TMP/harness" open-out "$FIX" </dev/null >"$TMP/out" 2>"$TMP/err")
got=$?
if [ "$got" -eq 3 ] && grep -q "AETHER_DENIED: fs_read $FIX/outside/secret.txt" "$FIX/outside/aether-sandbox.log" 2>/dev/null; then
    echo "  [PASS] a refusal logged where the grants do not reach is EACCES, and logged once"
else
    echo "  [FAIL] a refusal logged to a log the grants do not reach: exit $got, expected 3 and one line in its log"
    sed 's/^/    /' "$TMP/err" | head -5
    fail=1
fi
if [ -f "$FIX/outside/aether-sandbox.log" ] &&
   [ "$(grep -c AETHER_DENIED "$FIX/outside/aether-sandbox.log")" -ne 1 ]; then
    echo "  [FAIL] the log holds more than the one refusal:"
    head -5 "$FIX/outside/aether-sandbox.log" | sed 's/^/    /'
    fail=1
fi
rm -f "$FIX/outside/aether-sandbox.log"
# unshare(CLONE_NEWUSER|CLONE_NEWNS): EPERM from the preload, whatever the
# kernel would allow. The log tells the preload's refusal from the kernel's.
(cd "$FIX/box/sub" && LD_PRELOAD="$LIB" AETHER_SANDBOX_GRANTS="$TMP/box.grants" \
    AETHER_SANDBOX_LOG=stderr "$TMP/harness" unshare-ns "$FIX" </dev/null >"$TMP/out" 2>"$TMP/err")
got=$?
if [ "$got" -eq 10 ] && grep -q 'syscall:unshare' "$TMP/err"; then
    echo "  [PASS] a new user and mount namespace is refused without a native grant"
elif [ "$got" -eq 7 ]; then
    echo "  [SKIP] unshare: not on this system"
else
    echo "  [FAIL] unshare(CLONE_NEWUSER|CLONE_NEWNS) under the preload: exit $got, expected 10 and the refusal logged"
    sed 's/^/    /' "$TMP/err"
    fail=1
fi

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
if [ -e "$FIX/box/slash_new" ] || [ ! -e "$FIX/box/inside.txt" ] || [ ! -L "$FIX/box/out_dir" ] ||
   [ -e "$FIX/box/rmd" ] || [ ! -e "$FIX/box/ren_s.txt" ]; then
    echo "  [FAIL] a call on 'name/' acted on the name as something else"
    fail=1
fi
if [ -e "$FIX/box/stolen" ] || [ -e "$FIX/box/stolen2" ] || [ -e "$FIX/box/hard" ] || [ -e "$FIX/box/stolen_fd" ]; then
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
