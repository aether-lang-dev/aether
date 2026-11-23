# find_msvcrt_gcc ROOT: prints the gcc of an msvcrt MinGW toolchain kept for
# tests, such as the msvcrt WinLibs (GCC 14.2, mingw-w64 12): the one
# $AETHER_TEST_MSVCRT_GCC names, else mingw64/bin/gcc.exe unpacked under
# build/scratch/.winlibs-msvcrt of the tree ROOT or of the main checkout ROOT
# is a worktree of. It must run and its headers must target msvcrt (no
# _UCRT). Prints nothing when there is none. For tests that build with that
# gcc as AE_CC and skip without it.
find_msvcrt_gcc() {
    _fm_roots="$1"
    _fm_common="$(git -C "$1" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)"
    # Without git: a worktree's .git file names <main>/.git/worktrees/<name>.
    if [ -z "$_fm_common" ] && [ -f "$1/.git" ]; then
        _fm_common="$(sed -n 's/^gitdir: \(.*\)\/worktrees\/[^/]*$/\1/p' "$1/.git")"
    fi
    [ -n "$_fm_common" ] && _fm_roots="$_fm_roots $(dirname "$_fm_common")"
    _fm_cands="${AETHER_TEST_MSVCRT_GCC:-}"
    for _fm_r in $_fm_roots; do
        _fm_cands="$_fm_cands $_fm_r/build/scratch/.winlibs-msvcrt/mingw64/bin/gcc.exe"
    done
    for _fm in $_fm_cands; do
        [ -f "$_fm" ] || continue
        "$_fm" -dumpversion > /dev/null 2>&1 || continue
        _fm_crt="$(printf '#include <stdlib.h>\n#ifdef _UCRT\nucrt\n#else\nmsvcrt\n#endif\n' |
            "$_fm" -E -P -x c - 2>/dev/null | tr -d '\r' | grep -x 'ucrt\|msvcrt')"
        [ "$_fm_crt" = msvcrt ] && { printf '%s\n' "$_fm"; return 0; }
    done
    return 0
}
