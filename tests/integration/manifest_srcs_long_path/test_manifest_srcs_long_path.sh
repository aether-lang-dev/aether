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

# The build has to run FROM the long root, and ae takes its root from where its
# own binary sits: a tree's build/ae is in dev mode, which AETHER_HOME does not
# override. So ae runs as $DEEP/build/ae, and that path has to stay long when
# ae asks the system for it.
#   - Windows: one directory junction for the whole tree. GetModuleFileName
#     reports the path ae was started by, junction and all. A symbolic link
#     needs a privilege (or Developer Mode), and without one MSYS2's `ln -s`
#     silently copies the whole tree instead. MSYS2 removes a junction as a
#     link, so the trap's rm -rf never reaches the tree behind it.
#   - POSIX: /proc/self/exe and realpath() resolve symbolic links, so a link
#     to the whole tree hands ae its short path back. $DEEP and $DEEP/build
#     are real directories instead, ae is a copy, and everything else is a
#     symbolic link into the tree.
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        mkdir -p "$(dirname "$DEEP")" &&
            MSYS2_ARG_CONV_EXCL='*' cmd /c mklink /J "$(cygpath -w "$DEEP")" "$(cygpath -w "$ROOT")" >/dev/null 2>&1
        ;;
    *)
        mkdir -p "$DEEP/build" && cp "$ROOT/build/ae" "$DEEP/build/ae" && {
            linked=0
            for e in "$ROOT"/*; do
                [ "$(basename "$e")" = build ] && continue
                ln -s "$e" "$DEEP/$(basename "$e")" || linked=1
            done
            for e in "$ROOT"/build/*; do
                [ "$(basename "$e")" = ae ] && continue
                ln -s "$e" "$DEEP/build/$(basename "$e")" || linked=1
            done
            [ "$linked" = 0 ]
        }
        ;;
esac
link_rc=$?
if [ "$link_rc" -ne 0 ] || [ ! -f "$DEEP/build/MANIFEST" ]; then
    echo "  [SKIP] manifest_srcs_long_path: cannot reach the tree through a long path"
    exit 0
fi

# A fresh cache: a binary cached from a build at the short root would be
# served as is, and nothing would be compiled from the long one.
if ! AETHER_CACHE_DIR="$TMP/cache" "$DEEP/build/ae" build -v --trace "$PROBE" -o "$TMP/probe" >"$TMP/build.log" 2>&1; then
    echo "  [FAIL] manifest_srcs_long_path: source build failed from the root at $DEEP"
    grep -E 'error|Undefined|undefined reference' "$TMP/build.log" | sed 's/^/        /' | head -8
    exit 1
fi

# The root ae actually built from, as it reports it. A short one here means
# the long path was resolved away and nothing above tested it.
deep_root=$(tr -d '\r' < "$TMP/build.log" | sed -n 's/^\[toolchain\] root: //p' | head -1)
case "$deep_root" in
    *cccccccccccccccccccc*root) ;;
    *)
        echo "  [FAIL] manifest_srcs_long_path: ae built from '$deep_root', not from the long root"
        exit 1
        ;;
esac
srcs=$(grep -vc '^#' "$ROOT/build/MANIFEST" 2>/dev/null || echo 0)
need=$(awk -v base="${#deep_root}" '!/^#/ && NF { n += length($0) + base + 4 } END { print n+0 }' \
       "$ROOT/build/MANIFEST")

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

echo "  [PASS] manifest_srcs_long_path: $srcs sources ($need bytes) build from a ${#deep_root}-char root"
