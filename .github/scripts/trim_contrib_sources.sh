#!/bin/sh
# Removes the C and Objective-C sources from an installed contrib tree
# (share/aether/contrib), keeping the ones a program still compiles from
# source there:
#
#   - every file a module.ae in the tree names with @source("..."), resolved
#     against that module's directory: contrib.vulkan and contrib.vulkan.vk
#     compile aether_vulkan.c into the program instead of linking an archive,
#     as contrib.d3d12 and contrib.metal do with theirs (#2208);
#   - contrib/sqlite/aether_sqlite.c and the fetched SQLite amalgamation,
#     which `ae build --target` compiles for the target (#1372);
#   - with --keep-host-bridges, contrib/host/<lang>/aether_host_<lang>.c,
#     which a plain `make install` ships because it builds no
#     libaether_host_<lang>.a for a downstream `import contrib.host.<lang>`
#     to link against (docs/install-layout.md, "What does NOT ship").
#
# Everything else compiled into the libaether_<x>.a archives that
# `make contrib` builds, so shipping it would be noise. Used by install.sh
# and the Makefile's install and install-contrib targets.
#
#   trim_contrib_sources.sh <installed contrib dir> [--keep-host-bridges]
set -eu

dir="${1:?usage: trim_contrib_sources.sh <contrib dir> [--keep-host-bridges]}"
keep_host=0
[ "${2:-}" = "--keep-host-bridges" ] && keep_host=1
[ -d "$dir" ] || exit 0

# Physical paths on both sides: the install prefix may sit behind a symlink,
# and a @source path may climb out of the module's directory ("../x.c").
physical() {
    ( cd "$(dirname "$1")" 2>/dev/null && printf '%s/%s\n' "$(pwd -P)" "$(basename "$1")" )
}

keep="$(mktemp)"
trap 'rm -f "$keep"' EXIT
# Any .ae can carry @source, not just module.ae: contrib.jq compiles
# aether_jq.c in with @source from value.ae, a sibling of its module.ae.
# Scanning module.ae alone deleted that .c and left an installed jq that
# could not build (#2208). @source resolves against the file's OWN
# directory, so keep dirname per matching .ae, not per module.
find "$dir" -type f -name '*.ae' | while IFS= read -r aef; do
    aedir="$(dirname "$aef")"
    sed -n 's/^[[:space:]]*@source("\([^"]*\)").*/\1/p' "$aef" | while IFS= read -r rel; do
        physical "$aedir/$rel" >> "$keep" || true
    done
done

find "$dir" -type f \( -name '*.c' -o -name '*.m' \) | while IFS= read -r f; do
    if [ "$keep_host" = 1 ]; then
        case "$f" in */contrib/host/*/aether_host_*.c) continue ;; esac
    fi
    # `ae build --target` compiles contrib.sqlite's veneer and the fetched
    # amalgamation for the target from these (#1372); no archive covers a
    # foreign target, so they ship as source.
    case "$f" in
        */contrib/sqlite/aether_sqlite.c|*/contrib/sqlite/amalgamation/*.c) continue ;;
    esac
    abs="$(physical "$f")" || abs="$f"
    if ! grep -qxF -- "$abs" "$keep"; then
        rm -f "$f"
    fi
done
