# find_zig ROOT: prints the first zig that answers `zig version`, on PATH or
# unpacked under build/scratch/.zig of the tree ROOT or of the main checkout
# ROOT is a worktree of; prints nothing when there is none. For tests that
# build with AE_CC="zig cc" and skip without it.
find_zig() {
    _fz_roots="$1"
    _fz_common="$(git -C "$1" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)"
    # Without git: a worktree's .git file names <main>/.git/worktrees/<name>.
    if [ -z "$_fz_common" ] && [ -f "$1/.git" ]; then
        _fz_common="$(sed -n 's/^gitdir: \(.*\)\/worktrees\/[^/]*$/\1/p' "$1/.git")"
    fi
    [ -n "$_fz_common" ] && _fz_roots="$_fz_roots $(dirname "$_fz_common")"
    for _fz in $(command -v zig 2>/dev/null); do
        "$_fz" version > /dev/null 2>&1 && { printf '%s\n' "$_fz"; return 0; }
    done
    for _fz_r in $_fz_roots; do
        for _fz in "$_fz_r"/build/scratch/.zig/zig-*/zig.exe "$_fz_r"/build/scratch/.zig/zig-*/zig; do
            [ -f "$_fz" ] && "$_fz" version > /dev/null 2>&1 && { printf '%s\n' "$_fz"; return 0; }
        done
    done
    return 0
}
