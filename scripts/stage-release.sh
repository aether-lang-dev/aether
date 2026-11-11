#!/bin/sh
# Stage the binary release tree: the one definition of what a release
# archive holds, used by every leg of .github/workflows/release.yml and by
# `make test-release-archive`, so what CI checks is what ships.
#
#   scripts/stage-release.sh <bin-dir> <out-dir> [--module-artifacts <aetherc>]
#                            [--contrib-archives | --contrib-target <triple>]
#
# <bin-dir> holds the built aetherc and ae (.exe on Windows), libaether.a,
# MANIFEST and, when built, shared/ (build/, or build/.alien/<target> for a
# cross-build). <out-dir> is created and receives bin/, lib/, include/aether/,
# share/aether/, LICENSE, README.md and VERSION; the caller packs it.
# --module-artifacts runs <aetherc> to precompile the installed std modules
# (#1746); a cross-build, whose aetherc cannot run here, leaves it out and its
# imports parse the sources.
# --contrib-archives builds contrib.sqlite's veneer and the vendored SQLite
# for the machine running this script and ships them as lib/contrib/
# libaether_sqlite.a and libsqlite3.a (the dev layout's build/contrib, which
# `ae build` and aeb's own link both put on -L). Only a NATIVE stage may pass
# it: a cross-build's archives would be for the wrong machine.
# --contrib-target <triple> builds the same two archives for a cross-built
# release instead, with `zig cc -target <triple>` (zig on PATH, and for a
# FreeBSD triple the base sysroot in AETHER_SYSROOT, as the cross-build
# itself takes them), -fPIC like the native ones, and checks each object is
# for that target. The FreeBSD release shipped without lib/contrib/ until
# its leg passed this, so `-laether_sqlite` failed to link against it.
#
# The packaging used to be written out three times in release.yml (Unix,
# Windows, FreeBSD) and a fourth time, differently, in the Makefile's smoke
# test. The copies drifted (libaether.h, #1420), and because the release
# builds a tag that can predate main's HEAD, a release.yml step naming a file
# the tag did not have failed every leg of 0.757.0. Here the packaging lives
# in the tree it packages.
set -eu

[ $# -ge 2 ] || { echo "usage: $0 <bin-dir> <out-dir> [--module-artifacts <aetherc>]" >&2; exit 2; }
bin="$1"; out="$2"; shift 2
modules_aetherc=""
contrib_archives=""
contrib_target=""
while [ $# -gt 0 ]; do
    case "$1" in
        --module-artifacts) [ $# -ge 2 ] || { echo "$0: --module-artifacts needs an aetherc" >&2; exit 2; }
                            modules_aetherc="$2"; shift 2 ;;
        --contrib-archives) contrib_archives=1; shift ;;
        --contrib-target)   [ $# -ge 2 ] || { echo "$0: --contrib-target needs a target triple" >&2; exit 2; }
                            contrib_archives=1; contrib_target="$2"; shift 2 ;;
        *) echo "$0: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

exe=""
[ -f "$bin/aetherc.exe" ] && exe=".exe"
[ -f "$bin/aetherc$exe" ] && [ -f "$bin/ae$exe" ] || {
    echo "$0: no aetherc$exe and ae$exe in $bin" >&2; exit 1; }
[ -s "$bin/MANIFEST" ] || { echo "$0: no MANIFEST in $bin; ae cannot build from source without it" >&2; exit 1; }

mkdir -p "$out/bin" "$out/lib" "$out/include/aether" "$out/share/aether"

cp "$bin/aetherc$exe" "$bin/ae$exe" "$out/bin/"
chmod 755 "$out/bin/"*
[ -f "$bin/libaether.a" ] && cp "$bin/libaether.a" "$out/lib/"
# Linux and FreeBSD: the sandbox's preload library, beside the archive, where
# spawn_sandboxed looks under AETHER_HOME (#3177).
[ -f "$bin/libaether_sandbox.so" ] && cp "$bin/libaether_sandbox.so" "$out/lib/"
# Windows: the --emit=obj entry point, beside the archive (aether_lib_main.c).
[ -f "$bin/libaether_main.a" ] && cp "$bin/libaether_main.a" "$out/lib/"
# The shared runtime (#2297): lib/shared/, beside the archive.
if [ -d "$bin/shared" ]; then
    mkdir -p "$out/lib/shared"
    cp "$bin/shared/"* "$out/lib/shared/"
fi

# Headers, as install.sh installs them: the public ones at the top of
# include/ (libaether.h, which runtime/libaether_caps.c includes, #1420), and
# every header under runtime/ and std/ with its directory kept, so a module
# added later is not left out of the archive.
cp include/*.h "$out/include/aether/"
for tree in runtime std; do
    (cd "$tree" && find . -name '*.h' -print) | while read -r h; do
        mkdir -p "$out/include/aether/$tree/$(dirname "$h")"
        cp "$tree/$h" "$out/include/aether/$tree/$h"
    done
done

# Runtime and std sources, for linking without libaether.a; MANIFEST lists
# which .c files to compile. std module specs (std/<mod>/test_*.ae, #1584) are
# tests, not payload.
cp -r runtime std "$out/share/aether/"
rm -rf "$out/share/aether/runtime/examples"
find "$out/share/aether/std" -type f -name 'test_*.ae' -delete
cp "$bin/MANIFEST" "$out/share/aether/"
# The package fetcher's source: `ae add` builds it with this toolchain the
# first time it fetches a package (#1362).
mkdir -p "$out/share/aether/tools/ae-fetch"
cp tools/ae-fetch/main.ae "$out/share/aether/tools/ae-fetch/"

# contrib sources, so `import contrib.X` resolves on a binary release as on a
# source install (#2208), trimmed as install.sh trims them: no tests,
# benchmarks, examples or build scripts; every module.ae, the host bridges,
# and the .c files a module.ae names with @source. The pinned SQLite
# amalgamation ships too (#1372): `ae build --target` compiles contrib.sqlite
# from it, and an installed toolchain must not need the network to do so. A
# failed fetch fails the staging.
sh scripts/fetch-sqlite-amalgamation.sh >/dev/null
# The same for contrib.quickjs: its pinned amalgamation ships, because
# aether_quickjs.c compiles it into every program that imports the module.
sh scripts/fetch-quickjs-amalgamation.sh >/dev/null
cp -r contrib "$out/share/aether/"
c="$out/share/aether/contrib"
find "$c" -type d \( -name tests -o -name benchmarks \) -prune -exec rm -rf {} +
find "$c" -type f \( -name 'example_*.ae' -o -name 'test_*.ae' -o -name 'test_*.sh' \
                     -o -name build.sh -o -name ci.sh \) -delete
sh .github/scripts/trim_contrib_sources.sh "$c" --keep-host-bridges
[ -f "$c/sqlite/amalgamation/sqlite3.c" ] || {
    echo "$0: the SQLite amalgamation is missing from the staged contrib" >&2; exit 1; }
[ -f "$c/quickjs/amalgamation/quickjs-amalgam.c" ] || {
    echo "$0: the QuickJS amalgamation is missing from the staged contrib" >&2; exit 1; }

# Compiled module artifacts (#1746), made by the aetherc this release ships.
if [ -n "$modules_aetherc" ]; then
    sh scripts/build_module_artifacts.sh "$modules_aetherc" . "$out/lib/aether/modules"
fi

# The contrib.sqlite archives (#1372). A program linked without `ae build`
# (aeb's orchestrator link, an aether.toml link_flags list) names
# `-laether_sqlite -lsqlite3` itself and reads no source, so a release that
# ships only the source (above) fails it with "library 'aether_sqlite' not
# found". `ae build` finds the same archives and keeps them in place of
# compiling the veneer. Native stages, or a cross stage with its target named
# (see the header).
if [ -n "$contrib_archives" ]; then
    cdir="$(mktemp -d)"
    if ! CONTRIB_TARGET="$contrib_target" MODULES=sqlite CONTRIB_OUT="$cdir" \
            bash tests/scripts/contrib_build.sh >"$cdir/build.log" 2>&1 \
       || [ ! -f "$cdir/libaether_sqlite.a" ] || [ ! -f "$cdir/libsqlite3.a" ]; then
        sed 's/^/    /' "$cdir/build.log" >&2
        echo "$0: could not build the contrib.sqlite archives${contrib_target:+ for $contrib_target} (the vendored libsqlite3.a is required)" >&2
        rm -rf "$cdir"; exit 1
    fi
    # A cross stage's archives must hold objects for the target: contrib_build
    # checks the veneer's, and this the vendored SQLite's as well.
    if [ -n "$contrib_target" ]; then
        # A relocatable object names no OS (FreeBSD's mark is the linker's),
        # so an ELF target is checked by its machine.
        case "$contrib_target" in
            *windows*)       want="COFF" ;;
            *macos*)         want="Mach-O" ;;
            x86_64-*)        want="ELF 64-bit LSB relocatable, x86-64" ;;
            aarch64-*)       want="ELF 64-bit LSB relocatable, ARM aarch64" ;;
            *)               want="ELF" ;;
        esac
        for a in libaether_sqlite.a libsqlite3.a; do
            xd="$cdir/x.$a"
            mkdir -p "$xd"
            (cd "$xd" && zig ar x "$cdir/$a") || { echo "$0: cannot unpack $a" >&2; rm -rf "$cdir"; exit 1; }
            for o in "$xd"/*.o; do
                if ! file -b "$o" | grep -q "$want"; then
                    echo "$0: $a holds $(basename "$o"), not a $want object: $(file -b "$o")" >&2
                    rm -rf "$cdir"; exit 1
                fi
            done
            rm -rf "$xd"
        done
    fi
    mkdir -p "$out/lib/contrib"
    cp "$cdir/libaether_sqlite.a" "$cdir/libsqlite3.a" "$out/lib/contrib/"
    rm -rf "$cdir"
fi

cp LICENSE README.md VERSION "$out/"
echo "$out"
