#!/bin/sh
# Issue #329 regression: `make install` (and install.sh) ship an
# authoritative MANIFEST listing link-suitable runtime + stdlib .c
# files for downstream consumers (aetherBuild and similar tools
# that compile against the share/aether/ source tree).
#
# Verifies:
#   1. install.sh writes share/aether/MANIFEST.
#   2. Every non-comment, non-empty line in MANIFEST resolves to a
#      file that actually exists under share/aether/. Catches drift
#      where Makefile RUNTIME_SRC / STD_SRC reference a path that
#      didn't get copied (or was trimmed by the rm -rf step).
#   3. The MANIFEST contains a sensible number of entries (>10).
#      Catches a silently-empty MANIFEST that would still "exist"
#      but be useless to consumers.
#   4. None of the trimmed paths (runtime/examples/, runtime/io/)
#      sneak into the MANIFEST. Catches drift the other direction
#      where a MANIFEST entry references content the trim removed.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

cd "$ROOT"
# The sweep has built the tree; install its binaries without rebuilding
# them (three drivers relinking build/ae in parallel collided, #2142).
if ! AETHER_INSTALL_NO_BUILD=1 ./install.sh "$TMPDIR" < /dev/null > "$TMPDIR/install.log" 2>&1; then
    echo "  [FAIL] install.sh exited non-zero"
    tail -20 "$TMPDIR/install.log"
    exit 1
fi

MANIFEST="$TMPDIR/share/aether/MANIFEST"

if [ ! -f "$MANIFEST" ]; then
    echo "  [FAIL] $MANIFEST not present after install"
    exit 1
fi

# Strip comments + blank lines, count entries.
nlines=$(grep -c -v -E '^(#|$)' "$MANIFEST" || true)
if [ "$nlines" -lt 10 ]; then
    echo "  [FAIL] MANIFEST has only $nlines real entries — looks broken"
    head -20 "$MANIFEST"
    exit 1
fi

# Every listed path must exist under share/aether/.
missing=0
while IFS= read -r path; do
    case "$path" in
        ''|'#'*) continue ;;  # comment / blank
    esac
    if [ ! -f "$TMPDIR/share/aether/$path" ]; then
        if [ "$missing" -lt 5 ]; then
            echo "  [MISSING] share/aether/$path (listed in MANIFEST but not present)"
        fi
        missing=$((missing + 1))
    fi
done < "$MANIFEST"

if [ "$missing" -ne 0 ]; then
    echo "  [FAIL] $missing MANIFEST entries don't resolve to a real file"
    exit 1
fi

# Trim guarantees — neither runtime/examples/ nor runtime/io/ should
# appear in MANIFEST (those dirs are explicitly removed by the
# install step + are never link-suitable).
if grep -E '^runtime/(examples|io)/' "$MANIFEST" >/dev/null; then
    echo "  [FAIL] MANIFEST references trimmed paths (runtime/examples or runtime/io):"
    grep -E '^runtime/(examples|io)/' "$MANIFEST"
    exit 1
fi

echo "  [PASS] MANIFEST authoritative after install ($nlines entries, all resolve) — issue #329"
