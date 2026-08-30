#!/bin/sh
# `ae version remove` / `gc` / `dedupe` — pruning and deduplicating the store.
#
# The store grows by a full release every install, and roughly 70% of it is
# byte-identical across versions. These three commands prune and share it.
#
# The centre of gravity is what must NEVER happen:
#   - removing the version the user is actually running (`current`, or the
#     `active_version` pin -- the two can disagree, so both are checked)
#   - `gc --keep N` deleting the wrong releases because the versions were
#     sorted as strings, which puts v0.9.0 above v0.10.0
#   - dedupe altering file CONTENT, or reporting space it did not reclaim
#
# Asserts:
#   - `installed` lists what is on disk and marks current/pinned
#   - `remove` refuses the current version and the pinned version
#   - `remove` deletes an unused version and leaves the others intact
#   - version ordering is numeric, not lexicographic (v0.10.0 > v0.9.0)
#   - `gc --keep N` keeps the newest N and never the wrong ones
#   - `gc --dry-run` removes nothing
#   - `dedupe` leaves every file byte-identical
#   - `dedupe` is idempotent: a second run finds nothing to share

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] version_gc_dedupe: ae not built"; exit 0; }

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP" || :; return 0; }
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

# A fake store. These need not be real installs for remove/gc/list -- the
# commands work on directory names and file contents -- so this stays fast
# and does not depend on the network.
VS="$TMP/home/.aether/versions"
mkdir -p "$VS"
# Deliberately spanning the 9/10 boundary: a string sort ranks v0.9.0 above
# v0.10.0 and would make `gc --keep 2` delete the two NEWEST releases.
for v in v0.9.0 v0.10.0 v0.11.0 v0.2.0; do
    mkdir -p "$VS/$v/bin" "$VS/$v/include/aether" "$VS/$v/share/aether"
    echo "binary for $v" > "$VS/$v/bin/ae"
    # >=4096 bytes so dedupe considers it, and identical across versions so
    # there is something to share.
    yes "shared header content" | head -400 > "$VS/$v/include/aether/common.h"
    cp "$VS/$v/include/aether/common.h" "$VS/$v/share/aether/common.h"
    echo "unique to $v" > "$VS/$v/bin/unique.txt"
done
# How "the version in use" is recorded differs by platform: a `current`
# symlink on Unix, and on Windows the ~/.aether/active_version pin, because
# `ae version use` copies into ~/.aether/bin/ rather than symlinking. Use
# whichever this platform actually reads, so the guard is tested for real
# rather than skipped.
IN_USE_KIND="current"
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) IN_USE_KIND="pin" ;;
esac
if [ "$IN_USE_KIND" = "current" ]; then
    ln -s "$VS/v0.11.0" "$TMP/home/.aether/current"
else
    echo "0.11.0" > "$TMP/home/.aether/active_version"
fi

# The home directory is USERPROFILE on Windows (get_home_dir checks it
# BEFORE HOME) and HOME elsewhere, so both must be set or the Windows leg
# reads the runner's real profile, finds no versions, and every assertion
# below fails. Same pattern as version_identity.
HOME="$TMP/home"; USERPROFILE="$TMP/home"; export HOME USERPROFILE

# --- installed ------------------------------------------------------------
OUT="$("$AE" version installed 2>&1)" || fail "version installed exited non-zero"
echo "$OUT" | grep -q "v0.11.0" || fail "installed did not list v0.11.0"
echo "$OUT" | grep -q "v0.2.0"  || fail "installed did not list v0.2.0"
if [ "$IN_USE_KIND" = "current" ]; then
    echo "$OUT" | grep "v0.11.0" | grep -q "current" \
        || fail "installed did not mark the current version"
else
    echo "$OUT" | grep "v0.11.0" | grep -q "pinned" \
        || fail "installed did not mark the pinned version"
fi

# Numeric ordering: v0.11.0 must come before v0.9.0 in the newest-first list.
ORDER="$(echo "$OUT" | grep -o 'v0\.[0-9]*\.0' | head -4 | tr '\n' ' ')"
case "$ORDER" in
    "v0.11.0 v0.10.0 v0.9.0 v0.2.0 "*) : ;;
    *) fail "versions not ordered numerically (got: $ORDER)" ;;
esac

# --- remove refuses the version in use -------------------------------------
if "$AE" version remove 0.11.0 >"$TMP/out" 2>&1; then
    fail "remove allowed deleting the version in use"
fi
grep -qi "refus" "$TMP/out" || fail "remove did not explain why it refused"
[ -d "$VS/v0.11.0" ] || fail "remove deleted the version in use anyway"

# --- remove refuses the pinned version ------------------------------------
# On Unix the pin is a SECOND thing to protect: it and `current` can name
# different versions, and both must be refused. On Windows the pin is the
# only mechanism, and v0.11.0 above already covered it.
if [ "$IN_USE_KIND" = "current" ]; then
    echo "0.9.0" > "$TMP/home/.aether/active_version"
    if "$AE" version remove 0.9.0 >"$TMP/out" 2>&1; then
        fail "remove allowed deleting the pinned version"
    fi
    [ -d "$VS/v0.9.0" ] || fail "remove deleted the pinned version anyway"
    rm -f "$TMP/home/.aether/active_version"
fi

# --- remove deletes an unused version -------------------------------------
"$AE" version remove 0.2.0 >"$TMP/out" 2>&1 || fail "remove of an unused version failed"
[ -d "$VS/v0.2.0" ] && fail "remove did not delete v0.2.0"
[ -d "$VS/v0.9.0" ] || fail "remove took an unrelated version with it"
[ -d "$VS/v0.11.0" ] || fail "remove took the current version with it"

# a version that is not installed is an error, not a silent success
if "$AE" version remove 0.99.0 >/dev/null 2>&1; then
    fail "remove reported success for a version that is not installed"
fi

# --- gc --dry-run removes nothing -----------------------------------------
BEFORE="$(ls "$VS" | sort | tr '\n' ' ')"
"$AE" version gc --keep 1 --dry-run >"$TMP/out" 2>&1 || fail "gc --dry-run failed"
AFTER="$(ls "$VS" | sort | tr '\n' ' ')"
[ "$BEFORE" = "$AFTER" ] || fail "gc --dry-run deleted something ($BEFORE -> $AFTER)"
grep -qi "would remove" "$TMP/out" || fail "gc --dry-run did not say what it would do"

# --- gc keeps the newest N, numerically -----------------------------------
"$AE" version gc --keep 2 >"$TMP/out" 2>&1 || fail "gc failed"
[ -d "$VS/v0.11.0" ] || fail "gc removed v0.11.0, one of the two newest"
[ -d "$VS/v0.10.0" ] || fail "gc removed v0.10.0 -- versions sorted as strings?"
[ -d "$VS/v0.9.0" ] && fail "gc kept v0.9.0 despite --keep 2"

# --keep 0 would delete everything: rejected rather than obeyed
if "$AE" version gc --keep 0 >/dev/null 2>&1; then
    fail "gc accepted --keep 0"
fi

# --- dedupe preserves content ---------------------------------------------
# Rebuild a store with known content so identity can be checked exactly.
rm -rf "$VS"
mkdir -p "$VS"
for v in v0.20.0 v0.21.0; do
    mkdir -p "$VS/$v/include/aether" "$VS/$v/share/aether"
    yes "identical across both versions" | head -400 > "$VS/$v/include/aether/big.h"
    cp "$VS/$v/include/aether/big.h" "$VS/$v/share/aether/big.h"
    echo "distinct in $v" > "$VS/$v/include/aether/small.txt"
done
cp -a "$VS" "$TMP/pristine"

"$AE" version dedupe >"$TMP/out" 2>&1 || fail "dedupe failed"

# Every file byte-identical afterwards. This is the assertion that matters:
# a dedupe that saves space by changing content is a corruption, not a win.
diff -r "$TMP/pristine" "$VS" >/dev/null 2>&1 \
    || fail "dedupe altered file content"

# --- dedupe is idempotent -------------------------------------------------
# A second run must find nothing. Without a shared-extent check, reflinks
# (which give each copy a NEW inode) make every run re-link the same files
# and re-report the same saving while freeing nothing.
"$AE" version dedupe >"$TMP/out2" 2>&1 || fail "second dedupe failed"
if grep -qi "Reclaimed" "$TMP/out2"; then
    grep -qi "no duplicate content" "$TMP/out2" \
        || fail "second dedupe re-reported a saving it did not make"
fi
diff -r "$TMP/pristine" "$VS" >/dev/null 2>&1 \
    || fail "second dedupe altered file content"

echo "  [PASS] version_gc_dedupe: remove/gc guards, numeric ordering, dedupe integrity"
