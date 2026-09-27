#!/bin/sh
# Regression: `make install` / `install.sh` ship the contrib host
# bridge sources so a plain installed tree is link-suitable for
# downstream apps that `import contrib.host.<lang>`.
#
# Before this carve-out, the install ran `find contrib -name '*.c'
# -delete` which silently removed the bridge .c files alongside
# the other contrib .c sources. The .ae module descriptors and
# .h headers stayed, so the resolver was happy — but the link
# step then had no `aether_host_<lang>_*` symbols to satisfy.
#
# Plain `make install` does NOT also build/install
# `libaether_host_<lang>.a` (that's a separate opt-in step via
# `make install-contrib`, gated on the per-language dev libraries
# being present). So the only thing that makes a plain install
# link-suitable for host bridges is the source files themselves.
#
# Verifies:
#   1. install.sh ships every contrib/host/<lang>/aether_host_<lang>.c
#      that exists in the source tree.
#   2. Every .c a module.ae names with @source ships too (#2208), and
#      no OTHER contrib .c file slips through (the carve-out is
#      surgical — broad `find -name '*.c'` cleanup still runs).
#   3. The companion .h header and module.ae descriptor are
#      present too (otherwise compiling the bridge would fail).

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

# Source-tree inventory: every aether_host_<lang>.c that exists.
src_bridges=$(find contrib/host -name 'aether_host_*.c' -type f | sort)
src_count=$(echo "$src_bridges" | grep -c .)

if [ "$src_count" -lt 1 ]; then
    echo "  [FAIL] source tree has no contrib/host/*/aether_host_*.c bridges — bug in the test"
    exit 1
fi

# Each one must be present in the install tree.
missing=0
for src in $src_bridges; do
    inst="$TMPDIR/share/aether/$src"
    if [ ! -f "$inst" ]; then
        if [ "$missing" -lt 5 ]; then
            echo "  [MISSING] $src not in install tree"
        fi
        missing=$((missing + 1))
    fi
done
if [ "$missing" -ne 0 ]; then
    echo "  [FAIL] $missing of $src_count host bridge .c files missing from install"
    exit 1
fi

# Companion .h headers and module.ae descriptors should be there too.
for src in $src_bridges; do
    dir="$TMPDIR/share/aether/$(dirname "$src")"
    base="aether_host_$(basename "$dir")"
    if [ ! -f "$dir/$base.h" ]; then
        echo "  [FAIL] $dir/$base.h missing"
        exit 1
    fi
    if [ ! -f "$dir/module.ae" ]; then
        echo "  [FAIL] $dir/module.ae missing"
        exit 1
    fi
done

# The other .c files that must ship: each one a module.ae in the installed
# tree compiles in with @source (contrib.vulkan, contrib.vulkan.vk,
# contrib.d3d12, contrib.metal), resolved against that module's directory,
# since an installed toolchain builds those modules from them (#2208).
inst="$TMPDIR/share/aether/contrib"
physical() {
    ( cd "$(dirname "$1")" 2>/dev/null && printf '%s/%s\n' "$(pwd -P)" "$(basename "$1")" )
}
sourced=$(find "$inst" -type f -name module.ae | while IFS= read -r mod; do
    d=$(dirname "$mod")
    sed -n 's/^[[:space:]]*@source("\([^"]*\)").*/\1/p' "$mod" | while IFS= read -r rel; do
        case "$rel" in *.c) physical "$d/$rel" ;; esac
    done
done | sort -u)
for f in $sourced; do
    if [ ! -f "$f" ]; then
        echo "  [FAIL] $f is named by a module.ae's @source but did not ship"
        exit 1
    fi
done
sourced_count=$(echo "$sourced" | grep -c . || true)

# No OTHER contrib .c file should have slipped through (the carve-out
# is surgical): what ships is the host bridges plus the @source'd files.
shipped_c=$(find "$inst" -type f -name '*.c' | sort)
extra=$(echo "$shipped_c" | while IFS= read -r f; do
    [ -n "$f" ] || continue
    case "$f" in */contrib/host/*/aether_host_*.c) continue ;; esac
    echo "$sourced" | grep -qxF -- "$(physical "$f")" && continue
    echo "$f"
done)
if [ -n "$extra" ]; then
    echo "  [FAIL] .c files in install tree that are neither host bridges nor @source'd (the carve-out is too wide):"
    echo "$extra" | head -5
    exit 1
fi

echo "  [PASS] $src_count host bridge sources and $sourced_count @source'd module sources ship; no other contrib .c leaked"
