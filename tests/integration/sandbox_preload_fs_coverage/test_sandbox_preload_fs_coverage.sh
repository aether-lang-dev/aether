#!/bin/sh
# Every file-system entry point of the C library is either wrapped by the
# LD_PRELOAD library or named here as one it deliberately leaves alone, with
# the reason (#3751). A call that is neither fails the test: before #3751
# unlink, rename, mkdir, symlink, link, chmod, truncate, creat, __open_2 and
# opendir went straight to libc under a policy.
#
# Two checks:
#   1. Each name in WRAPPED that this C library exports is exported by the
#      preload library too (a wrapper that was dropped, or never written).
#   2. On glibc, musl and FreeBSD's libc, each exported name that looks like
#      a path call (the FAMILY pattern below) is wrapped, a lookup, or not a
#      path, so a call a C library adds later is noticed rather than left
#      open.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LIB="$ROOT/build/libaether_sandbox.so"

os="$(uname -s 2>/dev/null)"
case "$os" in
    Linux|FreeBSD) ;;
    *)
        echo "  [SKIP] sandbox_preload_fs_coverage: preload is Linux and FreeBSD only"
        exit 0 ;;
esac
if [ ! -f "$LIB" ]; then
    echo "  [SKIP] sandbox_preload_fs_coverage: preload library not built"
    exit 0
fi
if ! command -v nm >/dev/null 2>&1; then
    echo "  [SKIP] sandbox_preload_fs_coverage: no nm"
    exit 0
fi

# The C library this system's programs load.
libc=""
for cand in "$(${CC:-cc} -print-file-name=libc.so.6 2>/dev/null)" \
            "$(${CC:-cc} -print-file-name=libc.so.7 2>/dev/null)" \
            /lib/libc.so.7 /lib64/libc.so.6 /lib/x86_64-linux-gnu/libc.so.6 \
            /lib/aarch64-linux-gnu/libc.so.6 /usr/lib/libc.so.6; do
    if [ -n "$cand" ] && [ -f "$cand" ] && nm -D "$cand" >/dev/null 2>&1; then
        libc="$cand"
        break
    fi
done
if [ -z "$libc" ]; then
    for cand in /lib/ld-musl-*.so.1; do
        [ -f "$cand" ] && libc="$cand" && break
    done
fi
if [ -z "$libc" ]; then
    echo "  [SKIP] sandbox_preload_fs_coverage: C library not found"
    exit 0
fi

TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT

# Defined, exported names, without symbol versions. The versions are asked
# for where nm takes the flag (FreeBSD's llvm-nm prints them anyway), so the
# C library's private names can be told apart: glibc's GLIBC_PRIVATE and
# FreeBSD's FBSDprivate_1.0 are for the C library's own family (libthr,
# rtld), not for programs. FreeBSD 15's libc.so.7 exports an underscored
# private alias of every system call it moved to libsys (_unlink, _rename,
# _execve), as glibc has its hidden __unlink; neither is a program's call,
# and the system calls behind them stay reachable through libsys's
# __sys_unlink and syscall(2) whatever this library wraps, which is what
# Landlock and Capsicum hold. The sweep (2) reads the public names; the
# wrapped list (1) reads them all, since _open and _openat, private too, are
# what FreeBSD's own fopen and opendir open through, and are wrapped.
{ nm -D --defined-only --with-symbol-versions "$libc" 2>/dev/null ||
  nm -D --defined-only "$libc" 2>/dev/null; } > "$TMP/libc.nm"
awk 'NF >= 3 { print $3 } NF == 2 { print $2 }' "$TMP/libc.nm" | sed 's/@.*//' | sort -u > "$TMP/libc.all"
grep -v -e 'GLIBC_PRIVATE' -e 'FBSDprivate' "$TMP/libc.nm" \
    | awk 'NF >= 3 { print $3 } NF == 2 { print $2 }' | sed 's/@.*//' | sort -u > "$TMP/libc.names"
nm -D --defined-only "$LIB" 2>/dev/null \
    | awk 'NF >= 3 { print $3 } NF == 2 { print $2 }' | sed 's/@.*//' | sort -u > "$TMP/preload.names"

# Wrapped: opened, listed, created, removed or changed by path, each
# checked and made on the path checked (libaether_sandbox_preload.c, "The
# file system"), and the program starts (exec, spawn, dlopen).
WRAPPED="
open open64 __open __open64 __open_2 __open64_2 openat openat64 __openat_2 __openat64_2
creat creat64 fopen fopen64 freopen freopen64
opendir scandir scandir64 scandirat scandirat64 glob glob64 nftw nftw64 ftw ftw64
fts_open fts_read fts64_open fts64_read
unlink unlinkat remove rmdir mkdir mkdirat mknod mknodat mkfifo mkfifoat
rename renameat symlink symlinkat link linkat
chmod fchmodat lchmod chown lchown fchownat truncate truncate64
utime utimes lutimes futimesat utimensat
mkstemp mkstemp64 mkostemp mkostemp64 mkstemps mkstemps64 mkostemps mkostemps64 mkdtemp
tmpfile tmpfile64
execve execv execvp execvpe execl execle execlp fexecve
posix_spawn posix_spawnp posix_spawn_file_actions_addopen dlopen
"
# Linux only: the calls (or their shapes) the BSDs do not share.
WRAPPED_LINUX="
mount umount umount2 renameat2 setxattr lsetxattr removexattr lremovexattr open_by_handle_at openat2
execveat pidfd_spawn pidfd_spawnp dlmopen __xmknod __xmknodat
"
# FreeBSD only: its file flags, extended attributes and ACLs set by path,
# its blocks forms, its mount calls, and libc's own exported open names.
WRAPPED_FREEBSD="
scandir_b scandirat_b mkostempsat chflags lchflags chflagsat funlinkat revoke undelete
mount unmount nmount extattrctl extattr_set_file extattr_set_link extattr_delete_file
extattr_delete_link acl_set_file acl_set_link_np acl_delete_def_file acl_delete_def_link_np
acl_delete_file_np acl_delete_link_np __acl_set_file __acl_set_link __acl_delete_file
__acl_delete_link exect execvP _open _openat renameat2 glob_b fts_open_b
"

# Lookups: they read no file's content and change nothing, so they are not
# wrapped (docs/containment-sandbox.md, "What each mode holds"). libc and
# the runtime make them for their own needs, and the wrappers' resolution
# makes them itself. chdir, fchdir and chroot move where relative paths
# start; every call made from there is checked where it leads.
# name_to_handle_at gives a handle, which open_by_handle_at (wrapped) opens.
LOOKUPS="
stat stat64 lstat lstat64 fstatat fstatat64 statx __xstat __xstat64 __lxstat __lxstat64
__fxstatat __fxstatat64 access faccessat euidaccess eaccess readlink readlinkat realpath
canonicalize_file_name getxattr lgetxattr listxattr llistxattr statfs statfs64 statvfs
statvfs64 __statfs pathconf lpathconf name_to_handle_at chdir chroot open_tree mount_setattr
posix_spawn_file_actions_addchdir_np
extattr_get_file extattr_get_link extattr_list_file extattr_list_link
acl_get_file acl_get_link_np __acl_get_file __acl_get_link __acl_aclcheck_file
__acl_aclcheck_link acl_valid_file_np acl_valid_link_np acl_extended_file_np
acl_extended_file_nofollow_np acl_extended_link_np
"

# Names the FAMILY pattern finds that take no path: a descriptor, a stream,
# a terminal, a log, an fts or glob handle already opened, a kernel module
# (create_module), a link-level address (link_addr), an ACL in memory.
NOT_A_PATH="
open_memstream open_wmemstream openlog openpty fopencookie create_module
globfree globfree64 glob_pattern_p
fts_close fts_children fts_set fts64_close fts64_children fts64_set
fts_get_clientptr fts_get_stream fts_set_clientptr link_addr link_ntoa link_ntoa_r
"
# On Linux glibc keeps BSD names that only fail with ENOSYS there.
if [ "$os" = Linux ]; then
    NOT_A_PATH="$NOT_A_PATH chflags lchflags revoke"
fi

in_list() {
    case " $(echo "$2" | tr '\n' ' ') " in
        *" $1 "*) return 0 ;;
    esac
    return 1
}

# Which C library this is: glibc, musl or FreeBSD's.
flavour=other
if grep -qx 'gnu_get_libc_version' "$TMP/libc.names"; then
    flavour=glibc
elif [ "$os" = FreeBSD ]; then
    flavour=freebsd
elif [ "$os" = Linux ]; then
    flavour=musl
fi

fail=0
ALL_WRAPPED="$WRAPPED"
if [ "$os" = Linux ]; then ALL_WRAPPED="$WRAPPED $WRAPPED_LINUX"; fi
if [ "$os" = FreeBSD ]; then ALL_WRAPPED="$WRAPPED $WRAPPED_FREEBSD"; fi

# 1. Every wrapped name this libc has, the preload has.
missing=""
for name in $ALL_WRAPPED; do
    if grep -qx "$name" "$TMP/libc.all" && ! grep -qx "$name" "$TMP/preload.names"; then
        missing="$missing $name"
    fi
done
if [ -n "$missing" ]; then
    echo "  [FAIL] the C library has these and the preload does not wrap them:$missing"
    fail=1
else
    echo "  [PASS] every wrapped entry point this C library exports is wrapped"
fi

# 2. On glibc, musl and FreeBSD's libc, nothing that looks like a path call
#    is left unclassified.
FAMILY='^_*(open|creat|fopen|freopen|opendir|scandir|glob|n?ftw|fts|unlink|remove|rmdir|mkdir|mknod|mkfifo|rename|symlink|link|chmod|lchmod|fchmodat|chown|lchown|fchownat|truncate|utime|lutime|futimesat|utimensat|setxattr|lsetxattr|removexattr|lremovexattr|mkstemp|mkostemp|mkdtemp|tmpfile|stat|lstat|fstatat|statx|access|faccessat|euidaccess|eaccess|readlink|realpath|canonicalize_file_name|getxattr|lgetxattr|listxattr|llistxattr|statfs|statvfs|pathconf|lpathconf|name_to_handle_at|chdir|chroot|mount|umount|unmount|nmount|exec|fexecve|pidfd_spawn|dlopen|dlmopen|__xmknod|__xstat|__lxstat|__fxstatat|chflags|lchflags|funlinkat|revoke|undelete|extattr|acl_|__acl_)'
if [ "$flavour" != other ]; then
    unknown=""
    for name in $(grep -E "$FAMILY" "$TMP/libc.names"); do
        case "$name" in
            posix_spawnattr_*) continue ;;
            # An ACL or an extended attribute held in memory or reached
            # through a descriptor: no path.
            acl_*_fd|acl_*_fd_np|__acl_*_fd|extattr_*_fd) continue ;;
            extattr_namespace_to_string|extattr_string_to_namespace) continue ;;
        esac
        if in_list "$name" "$ALL_WRAPPED" || in_list "$name" "$LOOKUPS" || in_list "$name" "$NOT_A_PATH"; then
            continue
        fi
        case "$name" in
            # A fortified lookup (__readlink_chk, __realpath_chk) is its lookup.
            __*_chk)
                base="${name#__}"
                base="${base%_chk}"
                if in_list "$base" "$LOOKUPS"; then continue; fi ;;
            # The rest of FreeBSD's acl_* build and read ACLs in memory; the
            # ones that take a path are listed above.
            acl_*)
                case "$name" in
                    *_file*|*_link*) ;;
                    *) continue ;;
                esac ;;
        esac
        unknown="$unknown $name"
    done
    for name in $(grep -E '^posix_spawn_file_actions_' "$TMP/libc.names"); do
        case "$name" in
            # addchdir and addfchdir are POSIX.1-2024's names for the _np
            # pair (FreeBSD 15 has both): a chdir, as LOOKUPS says.
            posix_spawn_file_actions_addopen|posix_spawn_file_actions_addchdir_np) continue ;;
            posix_spawn_file_actions_addchdir|posix_spawn_file_actions_addfchdir) continue ;;
            posix_spawn_file_actions_init|posix_spawn_file_actions_destroy) continue ;;
            posix_spawn_file_actions_addclose|posix_spawn_file_actions_adddup2) continue ;;
            posix_spawn_file_actions_addfchdir_np|posix_spawn_file_actions_addclosefrom_np) continue ;;
            posix_spawn_file_actions_addtcsetpgrp_np) continue ;;
        esac
        unknown="$unknown $name"
    done
    if [ -n "$unknown" ]; then
        echo "  [FAIL] path calls neither wrapped nor classified ($flavour):$unknown"
        fail=1
    else
        echo "  [PASS] every path call this C library ($flavour) exports is wrapped or classified"
    fi
else
    echo "  [SKIP] the unclassified-name sweep knows glibc, musl and FreeBSD's libc"
fi

echo ""
echo "sandbox_preload_fs_coverage: $([ "$fail" -eq 0 ] && echo PASS || echo FAIL)"
[ "$fail" -eq 0 ]
