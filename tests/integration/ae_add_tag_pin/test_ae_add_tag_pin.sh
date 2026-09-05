#!/bin/sh
# `ae add <pkg>@<tag>` must pin the tag exactly, and a failed pin must leave
# nothing resolvable behind (report: ae-add-tag-pin-fails-but-leaves-main).
#
# The checkout ran through run_cmd, which posix_spawns argv with NO shell, so
# `cd "dir" && git checkout ... 2>/dev/null || ...` was handed to a program
# literally named `cd`. The whole line failed every time -- the tag pin never
# worked -- yet the clone (a lone `git clone`, no shell metacharacters)
# succeeded, so `ae add @tag` exited 1 having left a clone of the DEFAULT
# BRANCH in the cache. A later resolve then bound the dependency to that
# unpinned tree and went green against the wrong code.
#
# Hermetic: a local bare git repo with three tags, served over file:// via
# AE_RELEASE_BASE_URL. Nothing here touches the network or the real cache.
#
# Pinned properties:
#   1. @vX.Y.Z checks out exactly that tag (not tag+N-commits),
#   2. the bare form @X.Y.Z resolves too,
#   3. a nonexistent tag exits 1, LISTS the available tags (empty before),
#      and leaves NO directory in the cache,
#   4. a valid pin records the dependency in aether.toml.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"
[ -x "$AE" ] || { echo "  [SKIP] ae_add_tag_pin: ae not built"; exit 0; }
command -v git >/dev/null 2>&1 || { echo "  [SKIP] ae_add_tag_pin: git needed"; exit 0; }

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

# Build a fake forge: origin/<host>/<user>/<repo> is a bare repo with tags.
PKG="example.com/acme/widget"
WORK="$TMP/work"
BARE="$TMP/origin/$PKG"
mkdir -p "$WORK" "$BARE"
git init -q "$WORK"
( cd "$WORK"
  git config user.email t@t.test; git config user.name t
  echo "v1" > f.txt; git add f.txt; git commit -qm one; git tag v0.1.0
  echo "v2" > f.txt; git commit -aqm two; git tag v0.2.0
  echo "v3" > f.txt; git commit -aqm three; git tag v0.2.1
  echo "past-the-tag" > f.txt; git commit -aqm four    # default branch is now PAST v0.2.1
  git init -q --bare "$BARE"
  git push -q "$BARE" HEAD:refs/heads/main --tags ) || {
    echo "  [FAIL] ae_add_tag_pin: could not build fixture repo"; exit 1; }

# A consumer project, with HOME redirected so the real cache is untouched.
PROJ="$TMP/proj"
mkdir -p "$PROJ"
( cd "$PROJ" && "$AE" init consumer >/dev/null 2>&1 ) || {
    echo "  [FAIL] ae_add_tag_pin: ae init failed"; exit 1; }
CONS="$PROJ/consumer"
export HOME="$TMP/home"
mkdir -p "$HOME"
export AE_RELEASE_BASE_URL="file://$TMP/origin"
CACHE="$HOME/.aether/packages/$PKG"

# --- 1. @vX.Y.Z pins EXACTLY that tag ----------------------------------
( cd "$CONS" && "$AE" add "$PKG@v0.2.1" ) > "$TMP/add.log" 2>&1
if [ ! -d "$CACHE" ]; then
    echo "  [FAIL] ae_add_tag_pin: @v0.2.1 installed nothing"
    sed 's/^/    /' "$TMP/add.log" | head -8; exit 1
fi
desc=$(git -C "$CACHE" describe --tags 2>/dev/null)
if [ "$desc" != "v0.2.1" ]; then
    echo "  [FAIL] ae_add_tag_pin: @v0.2.1 landed at '$desc', not the tag"
    echo "         (a '-N-g<sha>' suffix means the default branch, not the pin)"
    exit 1
fi

# --- 4. and it is recorded in the manifest -----------------------------
grep -q "$PKG" "$CONS/aether.toml" || {
    echo "  [FAIL] ae_add_tag_pin: pin not written to aether.toml"; exit 1; }

# --- 2. the bare form resolves too -------------------------------------
rm -rf "$CACHE"
( cd "$CONS" && "$AE" add "$PKG@0.2.0" ) > "$TMP/add2.log" 2>&1
desc=$(git -C "$CACHE" describe --tags 2>/dev/null)
if [ "$desc" != "v0.2.0" ]; then
    echo "  [FAIL] ae_add_tag_pin: bare @0.2.0 landed at '$desc', not v0.2.0"
    sed 's/^/    /' "$TMP/add2.log" | head -8; exit 1
fi

# --- 3. a bad tag: exit 1, lists tags, leaves NOTHING ------------------
rm -rf "$CACHE"
( cd "$CONS" && "$AE" add "$PKG@v9.9.9" ) > "$TMP/bad.log" 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "  [FAIL] ae_add_tag_pin: a nonexistent tag exited 0"; exit 1
fi
if [ -d "$CACHE" ]; then
    echo "  [FAIL] ae_add_tag_pin: a failed pin LEFT a clone in the cache:"
    echo "         $(git -C "$CACHE" describe --tags --always 2>/dev/null) -- the reproducibility hole"
    exit 1
fi
# The available-versions list was empty before the fix; it must name the tags.
if ! grep -q "v0.2.1" "$TMP/bad.log"; then
    echo "  [FAIL] ae_add_tag_pin: 'Available versions:' did not list the real tags"
    sed 's/^/    /' "$TMP/bad.log" | head -10; exit 1
fi

echo "  [PASS] ae_add_tag_pin: tags pin exactly; a failed pin lists versions and leaves no cache"
