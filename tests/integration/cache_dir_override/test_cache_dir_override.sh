#!/bin/sh
# Regression for #1032 (half 1): AETHER_CACHE_DIR redirects the exe cache.
#
# The cache location was hard-wired to $HOME/.aether/cache, which a runner
# with a read-only $HOME (agent sandbox, hermetic CI) cannot use — and
# AETHER_HOME deliberately does not move it (toolchain root, not artifact
# dir). This asserts the override works, populates ONLY the override dir,
# and survives the read-only-home scenario that surfaced the issue.
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] $AE not built (run make first)"
    exit 0
fi

TMPDIR_T="$(mktemp -d)"
trap 'chmod -R u+w "$TMPDIR_T" 2>/dev/null; rm -rf "$TMPDIR_T"' EXIT

CACHE="$TMPDIR_T/cache-override"
# Unique program text -> unique cache key -> guaranteed cold cache.
TOKEN="cdo-$$-$(date +%s)"
cat > "$TMPDIR_T/prog.ae" <<AE
extern println(s: string)
main() { println("$TOKEN") }
AE

# 1. Override populates the override dir.
out=$(AETHER_CACHE_DIR="$CACHE" "$AE" run "$TMPDIR_T/prog.ae" 2>&1)
case "$out" in
    *"$TOKEN"*) ;;
    *) echo "  [FAIL] run with AETHER_CACHE_DIR did not produce program output:"
       echo "$out" | sed 's/^/        /'; exit 1 ;;
esac
count=$(ls "$CACHE" 2>/dev/null | wc -l)
if [ "$count" -lt 1 ]; then
    echo "  [FAIL] AETHER_CACHE_DIR dir is empty after a cache-eligible run"
    exit 1
fi

# 2. Second run is a cache hit from the override dir (no recompile needed;
#    we assert it still runs correctly and doesn't touch the default dir).
out2=$(AETHER_CACHE_DIR="$CACHE" "$AE" run "$TMPDIR_T/prog.ae" 2>&1)
case "$out2" in
    *"$TOKEN"*) ;;
    *) echo "  [FAIL] cache-hit rerun failed:"; echo "$out2" | sed 's/^/        /'; exit 1 ;;
esac

# 3. The read-only-home scenario: $HOME unwritable, cache redirected.
#    Without the override this fails at link time ("Read-only file system").
RO_HOME="$TMPDIR_T/ro-home"
mkdir -p "$RO_HOME"
chmod 555 "$RO_HOME"
out3=$(HOME="$RO_HOME" AETHER_CACHE_DIR="$CACHE" "$AE" run "$TMPDIR_T/prog.ae" 2>&1)
rc3=$?
chmod 755 "$RO_HOME"
if [ "$rc3" -ne 0 ]; then
    echo "  [FAIL] run with read-only HOME + AETHER_CACHE_DIR exited $rc3:"
    echo "$out3" | sed 's/^/        /'
    exit 1
fi
case "$out3" in
    *"$TOKEN"*) ;;
    *) echo "  [FAIL] read-only-home run produced wrong output:"
       echo "$out3" | sed 's/^/        /'; exit 1 ;;
esac

# 4. `ae cache` must mean the SAME directory `ae build` writes to. It composed
#    "$HOME/.aether/cache" by hand and so ignored the override: it reported on
#    a directory the build path was not using, and `ae cache clear` deleted the
#    contents of the wrong one. On a box where the override exists precisely
#    because $HOME is read-only, that is a destructive answer to a read-only
#    problem: it discards the real cache while claiming to clear this one.
#
#    The "was the default left alone" half uses a FAKE home with a sentinel in
#    it, never the real cache. The real one is shared with every other test in
#    the sweep, which runs in parallel, so its entry count moves under this
#    test for reasons that have nothing to do with the clear.
entries() { sed -n 's/^Cache: *\([0-9][0-9]*\) build.*/\1/p'; }

FAKE_HOME="$TMPDIR_T/fake-home"
mkdir -p "$FAKE_HOME/.aether/cache"
SENTINEL="$FAKE_HOME/.aether/cache/sentinel"
: > "$SENTINEL"

n=$(AETHER_CACHE_DIR="$CACHE" "$AE" cache 2>/dev/null | entries)
if [ "${n:-0}" -lt 1 ]; then
    echo "  [FAIL] ae cache reported ${n:-0} entries in the override after runs"
    echo "         that populated it -- it is reading some other directory"
    exit 1
fi

clear_out=$(HOME="$FAKE_HOME" USERPROFILE="$FAKE_HOME" \
            AETHER_CACHE_DIR="$CACHE" "$AE" cache clear 2>&1)
case "$clear_out" in
    *"$CACHE"*) ;;
    *) echo "  [FAIL] ae cache clear did not name the override directory:"
       echo "$clear_out" | sed 's/^/        /'; exit 1 ;;
esac

after=$(AETHER_CACHE_DIR="$CACHE" "$AE" cache 2>/dev/null | entries)
if [ "${after:-1}" -ne 0 ]; then
    echo "  [FAIL] the override still holds ${after} entries after clear"
    exit 1
fi

if [ ! -f "$SENTINEL" ]; then
    echo "  [FAIL] clearing the override emptied the DEFAULT cache instead"
    echo "         (the sentinel under \$HOME/.aether/cache is gone)"
    exit 1
fi

echo "  [PASS] AETHER_CACHE_DIR overrides the cache location (incl. read-only \$HOME),"
echo "         and ae cache reports and clears that same directory"
exit 0
