#!/bin/sh
# `ae add <pkg>@<tag>` must pin the tag exactly, and a failed pin must leave
# nothing resolvable behind (report: ae-add-tag-pin-fails-but-leaves-main).
#
# The history: the checkout ran through run_cmd, which posix_spawns argv with
# NO shell, so `cd "dir" && git checkout ...` was handed to a program named
# `cd`; the tag pin never worked, yet the clone succeeded, so `ae add @tag`
# exited 1 having left the DEFAULT BRANCH in the cache. Since #1362 there is
# no git at all: `ae add` takes the archive the forge serves for the tag, so
# what lands is the tag's tree by construction, and this pins that it stays so.
#
# Hermetic: a mirror laid out as `ae add` reads one
# (<mirror>/<pkg>/archive/<ref>.tar.gz, <mirror>/<pkg>/tags.json), served
# over file:// through AE_RELEASE_BASE_URL. Nothing here touches the network
# or the real cache. A `git` that fails and leaves a mark is first on PATH.
#
# Pinned properties:
#   1. @vX.Y.Z installs exactly that tag's tree, not the default branch's,
#   2. the bare form @X.Y.Z resolves too,
#   3. a nonexistent tag exits 1, LISTS the available tags, and leaves NO
#      directory in the cache,
#   4. a valid pin records the dependency and its hash in aether.toml,
#   5. git is never run.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"
[ -x "$AE" ] || { echo "  [SKIP] ae_add_tag_pin: ae not built"; exit 0; }
command -v tar >/dev/null 2>&1 || { echo "  [SKIP] ae_add_tag_pin: tar needed to build the fixture"; exit 0; }

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

fail() {
    echo "  [FAIL] ae_add_tag_pin: $1"
    [ -n "$2" ] && [ -f "$2" ] && sed 's/^/    /' "$2" | head -20
    exit 1
}

# A fake forge: one archive per tag, each with the single top directory a
# forge's archive has, and a default branch PAST the newest tag.
PKG="example.com/acme/widget"
ARCH="$TMP/mirror/$PKG/archive"
mkdir -p "$ARCH" "$TMP/src"
for spec in v0.1.0:v1 v0.2.0:v2 v0.2.1:v3 HEAD:past-the-tag; do
    tag=${spec%%:*}
    mkdir -p "$TMP/src/widget-$tag"
    printf '%s\n' "${spec#*:}" > "$TMP/src/widget-$tag/f.txt"
    ( cd "$TMP/src" && COPYFILE_DISABLE=1 tar -czf "$ARCH/$tag.tar.gz" "widget-$tag" ) \
        || fail "could not build the fixture archive for $tag"
done
printf '[{"name":"v0.2.1"},{"name":"v0.2.0"},{"name":"v0.1.0"}]\n' > "$TMP/mirror/$PKG/tags.json"

# A git that must never run.
mkdir -p "$TMP/bin"
printf '#!/bin/sh\ntouch "%s/git-ran"\nexit 1\n' "$TMP" > "$TMP/bin/git"
chmod +x "$TMP/bin/git"
PATH="$TMP/bin:$PATH"
export PATH

# get_home_dir() (tools/ae_cache.c) prefers USERPROFILE on Windows and only
# falls back to HOME, so exporting HOME alone leaves a native ae.exe reading
# and WRITING the real user profile. A native ae.exe also cannot resolve an
# MSYS /tmp/... path, hence cygpath.
export HOME="$TMP/home"
if command -v cygpath >/dev/null 2>&1; then
    USERPROFILE="$(cygpath -m "$TMP/home")"
    export AE_RELEASE_BASE_URL="file://$(cygpath -m "$TMP/mirror")"
else
    USERPROFILE="$TMP/home"
    export AE_RELEASE_BASE_URL="file://$TMP/mirror"
fi
export USERPROFILE
mkdir -p "$HOME"
CACHE="$HOME/.aether/packages/$PKG"

new_project() {
    p="$TMP/$1"
    mkdir -p "$p"
    printf '[package]\nname = "consumer"\nversion = "0.1.0"\n\n[dependencies]\n' > "$p/aether.toml"
    echo "$p"
}

# --- 1. @vX.Y.Z installs EXACTLY that tag ------------------------------
CONS="$(new_project one)"
( cd "$CONS" && "$AE" add "$PKG@v0.2.1" ) > "$TMP/add.log" 2>&1 \
    || fail "@v0.2.1 exited non-zero" "$TMP/add.log"
[ -d "$CACHE" ] || fail "@v0.2.1 installed nothing" "$TMP/add.log"
got=$(cat "$CACHE/f.txt" 2>/dev/null)
[ "$got" = "v3" ] || fail "@v0.2.1 landed '$got', not the tag's tree (past-the-tag is the default branch)" "$TMP/add.log"

# --- 4. and it is recorded, with its hash ------------------------------
grep -q "\"$PKG\" = \"v0.2.1\"" "$CONS/aether.toml" \
    || fail "pin not written to [dependencies]" "$CONS/aether.toml"
grep -q '^\[dependency-hashes\]' "$CONS/aether.toml" \
    || fail "no [dependency-hashes] section" "$CONS/aether.toml"
grep -q "\"$PKG\" = \"sha256-[0-9a-f]\{64\}\"" "$CONS/aether.toml" \
    || fail "the tree's hash was not pinned" "$CONS/aether.toml"

# --- 2. the bare form resolves too -------------------------------------
rm -rf "$CACHE" "$CACHE.fetched"
CONS="$(new_project two)"
( cd "$CONS" && "$AE" add "$PKG@0.2.0" ) > "$TMP/add2.log" 2>&1 \
    || fail "bare @0.2.0 exited non-zero" "$TMP/add2.log"
got=$(cat "$CACHE/f.txt" 2>/dev/null)
[ "$got" = "v2" ] || fail "bare @0.2.0 landed '$got', not v0.2.0's tree" "$TMP/add2.log"

# --- 3. a bad tag: exit 1, lists tags, leaves NOTHING ------------------
rm -rf "$CACHE" "$CACHE.fetched"
CONS="$(new_project bad)"
( cd "$CONS" && "$AE" add "$PKG@v9.9.9" ) > "$TMP/bad.log" 2>&1
rc=$?
[ "$rc" -ne 0 ] || fail "a nonexistent tag exited 0" "$TMP/bad.log"
[ ! -e "$CACHE" ] || fail "a failed pin LEFT a tree in the cache" "$TMP/bad.log"
[ ! -e "$CACHE.fetched" ] || fail "a failed pin left a fetch record" "$TMP/bad.log"
grep -q "v0.2.1" "$TMP/bad.log" \
    || fail "'Available versions:' did not list the real tags" "$TMP/bad.log"
grep -q "$PKG" "$CONS/aether.toml" \
    && fail "a failed pin was recorded in aether.toml" "$CONS/aether.toml"

# --- 5. no git ---------------------------------------------------------
[ ! -e "$TMP/git-ran" ] || fail "ae add ran git"

echo "  [PASS] ae_add_tag_pin: tags pin exactly, with their hash; a failed pin lists versions and leaves nothing; no git"
