#!/bin/sh
# The build cache must invalidate when aetherc itself changes, even when the
# rebuild lands in the same wall-clock second as the previous one.
#
# compute_cache_key folded the toolchain binaries (aetherc, the ae driver,
# libaether) in by st_mtime, which is SECOND-granularity. A `make` that
# rebuilds aetherc with different codegen followed by an `ae run` in the same
# second produced an identical key and served the binary the OLD compiler
# emitted -- a different codegen, reported as success, with every measurement
# against it silently wrong. The key now folds a CONTENT HASH of each binary,
# so any change in what aetherc produces is reflected regardless of mtime.
#
# Exercised end-to-end: build a program against a private copy of the
# toolchain (miss + publish), change aetherc's bytes while RESTORING its
# mtime (identical mtime, different content), run again, and assert the second
# run is a cache MISS. On the pre-fix code it was a hit.
#
# aetherc's content is changed by appending bytes it never reads for this
# program; ae only stat()s and hashes the file to key the cache, and the real
# compile still runs, so the program's behaviour is unchanged -- only the
# cache decision is under test.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*)
        # A same-second rebuild + run is the trigger; the fix (content hash) is
        # platform-neutral, but this test appends to a live .exe, which Windows
        # may lock. Skip rather than fight file locking on the CI leg.
        echo "  [SKIP] cache_compiler_invalidation: appends to a live aetherc binary (POSIX only)"
        exit 0 ;;
esac

if [ ! -x "$ROOT/build/ae" ] || [ ! -x "$ROOT/build/aetherc" ]; then
    echo "  [SKIP] cache_compiler_invalidation: build/ae or build/aetherc not built"
    exit 0
fi

WORK="$(mktemp -d)"
HOME_ISO="$(mktemp -d)"
CACHE="$(mktemp -d)"
trap 'rm -rf "$WORK" "$HOME_ISO" "$CACHE"' EXIT INT TERM

# A private, writable copy of the dev-mode toolchain: ae + aetherc side by
# side, with the repo's runtime/ and std/ and libaether reachable via a
# `runtime` marker dir so ae's "dev mode" root detection (exe_dir's parent has
# runtime/) picks THIS aetherc. We symlink the heavy dirs back to the repo.
mkdir -p "$WORK/tc/build"
cp "$ROOT/build/ae" "$WORK/tc/build/ae"
cp "$ROOT/build/aetherc" "$WORK/tc/build/aetherc"
cp "$ROOT/build/libaether.a" "$WORK/tc/build/libaether.a" 2>/dev/null || true
for d in runtime std include contrib; do
    [ -e "$ROOT/$d" ] && ln -s "$ROOT/$d" "$WORK/tc/$d"
done
AE="$WORK/tc/build/ae"
AETHERC="$WORK/tc/build/aetherc"

cat > "$WORK/prog.ae" <<'AEEOF'
main() { println("hello") }
AEEOF

fail() { echo "  [FAIL] cache_compiler_invalidation: $1"; exit 1; }

run() {  # prints the [cache] verbosity line (miss/hit) to stdout
    ( cd "$WORK" && HOME="$HOME_ISO" AETHER_CACHE_DIR="$CACHE" \
        "$AE" run --verbose prog.ae 2>&1 )
}

# Run 1: cold, must miss and print "hello".
out1="$(run)"
printf '%s\n' "$out1" | grep -q 'hello' || fail "run 1 did not run the program: $out1"
printf '%s\n' "$out1" | grep -qi '\[cache\] miss' || fail "run 1 was not a cold miss"

# Run 2: unchanged toolchain, must HIT (cache works; no over-invalidation).
out2="$(run)"
printf '%s\n' "$out2" | grep -qi '\[cache\] hit' || fail "run 2 (unchanged) did not hit the cache"

# Now change aetherc's CONTENT while keeping its mtime identical.
touch -r "$AETHERC" "$WORK/.mtref"
printf '\0aetherc-changed' >> "$AETHERC"
touch -r "$WORK/.mtref" "$AETHERC"     # identical mtime, different content+size

# Run 3: must MISS (the fix). On the pre-fix mtime key it was a stale hit.
out3="$(run)"
printf '%s\n' "$out3" | grep -q 'hello' || fail "run 3 did not run the program: $out3"
printf '%s\n' "$out3" | grep -qi '\[cache\] miss' \
    || fail "run 3 hit the cache despite a changed aetherc (same-mtime stale-serve bug)"

echo "  [PASS] cache_compiler_invalidation: a changed aetherc invalidates the cache even at an identical mtime"
exit 0
