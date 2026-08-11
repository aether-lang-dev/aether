#!/bin/sh
# Regression: a source build (no libaether.a, e.g. `ae build --trace`) must
# compile every source in MANIFEST no matter how long the tree's path is.
#
# The list lived in a fixed 8 KB buffer. 91 absolute paths need 6.0 KB under a
# 32-char prefix and 9.7 KB under a 73-char one, and on overflow the builder
# silently substituted a shorter hand-written list, so the link failed on
# whatever that list had drifted away from (std/bytes, most visibly) with no
# hint that the path length was the cause.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$ROOT" || exit 1

AE="$ROOT/build/ae"
PROBE="$ROOT/tests/integration/message_trace/probe.ae"
[ -x "$AE" ] || { echo "  [SKIP] manifest_srcs_long_path: build/ae not built"; exit 0; }
[ -f "$PROBE" ] || { echo "  [SKIP] manifest_srcs_long_path: probe.ae missing"; exit 0; }
[ -f "$ROOT/build/MANIFEST" ] || { echo "  [SKIP] manifest_srcs_long_path: no MANIFEST"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

DEEP="$TMP/aaaaaaaaaaaaaaaaaaaa/bbbbbbbbbbbbbbbbbbbb/cccccccccccccccccccc/root"
mkdir -p "$(dirname "$DEEP")" 2>/dev/null
if ! ln -s "$ROOT" "$DEEP" 2>/dev/null || [ ! -d "$DEEP/build" ]; then
    echo "  [SKIP] manifest_srcs_long_path: cannot create a symlink (needs privilege on Windows)"
    exit 0
fi

srcs=$(grep -vc '^#' "$ROOT/build/MANIFEST" 2>/dev/null || echo 0)
need=$(awk -v base="${#DEEP}" '!/^#/ && NF { n += length($0) + base + 4 } END { print n+0 }' \
       "$ROOT/build/MANIFEST")

if ! AETHER_HOME="$DEEP" "$AE" build --trace "$PROBE" -o "$TMP/probe" >"$TMP/build.log" 2>&1; then
    echo "  [FAIL] manifest_srcs_long_path: source build failed from a ${#DEEP}-char root"
    echo "         the list needs $need bytes for $srcs sources"
    grep -E 'error|Undefined|undefined reference' "$TMP/build.log" | sed 's/^/        /' | head -8
    exit 1
fi

out=$("$TMP/probe" 2>&1)
if [ "$out" != "ping 1
pong 2
ping 3" ]; then
    echo "  [FAIL] manifest_srcs_long_path: the binary built but misbehaved"
    printf '%s\n' "$out" | sed 's/^/        /' | head -6
    exit 1
fi

# The hand-written list is gone; a missing MANIFEST must be reported, never
# substituted for. Anchored on the sentinel that only that list contained.
if grep -q 'std/collections/aether_stringseq.c "' tools/ae.c; then
    echo "  [FAIL] manifest_srcs_long_path: a hand-written source list is back in tools/ae.c"
    echo "        MANIFEST is the single source of truth; it drifts if duplicated."
    exit 1
fi

echo "  [PASS] manifest_srcs_long_path: $srcs sources ($need bytes) build from a ${#DEEP}-char root"
