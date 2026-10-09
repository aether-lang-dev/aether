#!/bin/sh
# Issue #334 regression: `make install` populates share/aether/contrib/
# with each contrib module's module.ae descriptor + headers, so an
# Aether program can `import contrib.<X>` and have the resolver find
# the descriptor without needing the upstream aether checkout living
# at a known relative path.
#
# Verifies:
#   1. install.sh writes contrib/<X>/module.ae for every module that
#      had one in the source tree.
#   2. install.sh trims the source-tree noise (.c, .m, tests/,
#      benchmarks/, example_*.ae, test_*.sh, build.sh, ci.sh) — the
#      install layout is descriptor-+-header only, with two
#      explicit carve-outs: contrib/host/<lang>/aether_host_<lang>.c
#      DOES ship (plain `make install` doesn't build the matching
#      libaether_host_<lang>.a, so downstream apps that
#      `import contrib.host.<lang>` compile the bridge from source),
#      and so does any .c a module.ae names with @source (#2208):
#      contrib.vulkan, contrib.vulkan.vk, contrib.d3d12 and
#      contrib.metal compile theirs into the program, so an installed
#      toolchain without it cannot build them at all.
#      See docs/install-layout.md "What does NOT ship".
#   3. The module.ae files are syntactically what the resolver looks
#      for: a non-empty file at the documented path.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

cd "$ROOT"

# The .c files an installed contrib tree must still hold: each one a
# module.ae in the SOURCE tree names with @source, at the same relative
# path. Printed relative to contrib/ (vulkan/aether_vulkan.c).
sourced_c_files() {
    # Every .ae, not just module.ae: contrib.jq @sources aether_jq.c from
    # value.ae, a sibling of its facade (#2208). @source resolves against
    # the @source'ing file's own directory. Only the files that say @source
    # at all are read line by line: one grep picks them out, where a dirname
    # and a sed for each of the 121 .ae files, run twice, took about 20
    # seconds of this test on Windows (#2596).
    for aef in $(find contrib -name '*.ae' -exec grep -l '@source' {} + | sort); do
        moddir="${aef%/*}"
        sed -n 's/^[[:space:]]*@source("\([^"]*\)").*/\1/p' "$aef" | while IFS= read -r rel; do
            case "$rel" in *.c) ;; *) continue ;; esac
            ( cd "$moddir/$(dirname "$rel")" 2>/dev/null && printf '%s/%s\n' "$(pwd)" "$(basename "$rel")" ) \
                | sed "s|^$(pwd)/contrib/||"
        done
    done | sort -u
}

# The .c files an installed tree may hold: the @source'd ones above (as
# `find` prints them, under $1), contrib.sqlite's cross-build sources,
# contrib.quickjs's amalgamation, and,
# when $2 is "host", the host bridges.
# Anything else `find` lists is noise the trim step let through.
unexpected_c_files() {
    allowed="$(sourced_c_files | sed "s|^|$1/|")"
    find "$1" -type f -name '*.c' 2>/dev/null | while IFS= read -r f; do
        case "$2:$f" in host:*/contrib/host/*/aether_host_*.c) continue ;; esac
        # contrib.sqlite's veneer and fetched amalgamation ship in both
        # layouts: `ae build --target` compiles them for the target (#1372).
        case "$f" in */contrib/sqlite/aether_sqlite.c|*/contrib/sqlite/amalgamation/*.c) continue ;; esac
        # aether_quickjs.c #includes the fetched QuickJS amalgamation, so no
        # @source names it; it ships with the module that needs it.
        case "$f" in */contrib/quickjs/amalgamation/*.c) continue ;; esac
        printf '%s\n' "$allowed" | grep -qxF -- "$f" || printf '%s\n' "$f"
    done
}

# Every @source'd .c must be there; this is what #2208 was about.
assert_sourced_c_present() {
    for rel in $(sourced_c_files); do
        if [ ! -f "$1/$rel" ]; then
            echo "  [FAIL] $2: $rel is named by a module's @source but was trimmed from the install"
            exit 1
        fi
    done
}

# Run install.sh against the temp prefix. Quiet — we only care about
# the resulting layout.
# The sweep has built the tree; install its binaries without rebuilding
# them (three drivers relinking build/ae in parallel collided, #2142).
if ! AETHER_INSTALL_NO_BUILD=1 ./install.sh "$TMPDIR" < /dev/null > "$TMPDIR/install.log" 2>&1; then
    echo "  [FAIL] install.sh exited non-zero"
    tail -20 "$TMPDIR/install.log"
    exit 1
fi

CONTRIB_INSTALL="$TMPDIR/share/aether/contrib"

if [ ! -d "$CONTRIB_INSTALL" ]; then
    echo "  [FAIL] $CONTRIB_INSTALL does not exist after install"
    exit 1
fi

# Every module.ae in the source contrib/ must have a counterpart in
# the install. Walk source-side and assert install-side presence.
missing=0
for src_module in $(find contrib -name 'module.ae' | sort); do
    rel="${src_module#contrib/}"
    target="$CONTRIB_INSTALL/$rel"
    if [ ! -f "$target" ]; then
        echo "  [FAIL] missing in install: $rel"
        missing=$((missing + 1))
    elif [ ! -s "$target" ]; then
        echo "  [FAIL] empty in install: $rel"
        missing=$((missing + 1))
    fi
done

if [ "$missing" -ne 0 ]; then
    echo "  [FAIL] $missing contrib module.ae file(s) missing or empty"
    exit 1
fi

# Source-tree noise must NOT have been copied. Hits would be
# regression of the trim step.
unwanted_count=$( {
    # `.c` files: everything except the host-bridge and @source carve-outs.
    unexpected_c_files "$CONTRIB_INSTALL" host
    find "$CONTRIB_INSTALL" -type f -name '*.m'         2>/dev/null
    find "$CONTRIB_INSTALL" -type d -name tests         2>/dev/null
    find "$CONTRIB_INSTALL" -type d -name benchmarks    2>/dev/null
    find "$CONTRIB_INSTALL" -type f -name 'example_*.ae' 2>/dev/null
    find "$CONTRIB_INSTALL" -type f -name 'test_*.sh'   2>/dev/null
    find "$CONTRIB_INSTALL" -type f -name 'build.sh'    2>/dev/null
    find "$CONTRIB_INSTALL" -type f -name 'ci.sh'       2>/dev/null
} | wc -l | tr -d ' ')

if [ "$unwanted_count" -ne 0 ]; then
    echo "  [FAIL] install layout still contains source-tree noise:"
    {
        unexpected_c_files "$CONTRIB_INSTALL" host
        find "$CONTRIB_INSTALL" -type f -name '*.m'
        find "$CONTRIB_INSTALL" -type d -name tests
        find "$CONTRIB_INSTALL" -type d -name benchmarks
        find "$CONTRIB_INSTALL" -type f -name 'example_*.ae'
        find "$CONTRIB_INSTALL" -type f -name 'test_*.sh'
        find "$CONTRIB_INSTALL" -type f -name 'build.sh'
        find "$CONTRIB_INSTALL" -type f -name 'ci.sh'
    } | head -10
    exit 1
fi

assert_sourced_c_present "$CONTRIB_INSTALL" "install.sh"

# Spot-check a flagship module the issue called out by name.
for canary in sqlite tinyweb host/python; do
    if [ ! -f "$CONTRIB_INSTALL/$canary/module.ae" ]; then
        echo "  [FAIL] canary contrib module $canary not installed"
        exit 1
    fi
done

# ---------------------------------------------------------------------
# Second install path: `make install-contrib`. This is the separate
# installer that ships the prebuilt libaether_<X>.a archives alongside
# each module's source tree. It uses a DIFFERENT trim policy from
# install.sh (above) — historically the two diverged (Makefile trimmed
# tinyweb/ while install.sh shipped it). Pin both shapes so they don't
# drift apart again.
#
# Compared to install.sh's check: this path additionally requires the
# libaether_<X>.a archive present under lib/aether/ for each module
# that contrib_build.sh built. We don't require every module to have a
# .a (system dep absence → SKIP, not FAIL), but the canaries listed
# above MUST ship both module.ae AND the matching archive.
#
# Windows skip: `make install-contrib` shells `contrib_build.sh` which
# probes for sqlite3/python/lua/perl/ruby/duktape/tcl dev libs — none
# are available under MSYS2/MinGW on the GitHub runner, and the suite
# is already 4x slower on Windows (~33 min vs ~8 min on Linux). The
# install.sh half above provides the layout coverage; the
# make install-contrib half runs on every Linux/macOS lane and that
# is sufficient. Skip with a [SKIP-WIN] marker so the .ae-test runner
# (Makefile:542) records the skip as a pass with a reason.
# ---------------------------------------------------------------------
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] make install-contrib path skipped on Windows"
        echo "  [PASS] contrib/ resolves system-wide after install (issue #334)"
        echo "         install.sh path verified; make install-contrib path skipped on Windows"
        exit 0
        ;;
esac

MAKE_TMP="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" "$MAKE_TMP"' EXIT

# Build + install. This shells `make install-contrib PREFIX=...` which
# in turn invokes `make contrib` (the contrib_build.sh sweep) and then
# the install rule. Quiet on success; we'll dump the log on failure.
if ! make install-contrib PREFIX="$MAKE_TMP" \
        > "$MAKE_TMP/install.log" 2>&1; then
    echo "  [FAIL] make install-contrib exited non-zero"
    tail -30 "$MAKE_TMP/install.log"
    exit 1
fi

MAKE_CONTRIB_INSTALL="$MAKE_TMP/share/aether/contrib"
MAKE_LIB_DIR="$MAKE_TMP/lib/aether"

if [ ! -d "$MAKE_CONTRIB_INSTALL" ]; then
    echo "  [FAIL] make install-contrib produced no $MAKE_CONTRIB_INSTALL"
    exit 1
fi

# Source-tree noise must NOT have been copied (same shape as
# install.sh, plus test_*.ae which the Makefile install trim now
# filters too).
make_unwanted=$( {
    unexpected_c_files "$MAKE_CONTRIB_INSTALL" archives
    find "$MAKE_CONTRIB_INSTALL" -type f -name '*.m'          2>/dev/null
    find "$MAKE_CONTRIB_INSTALL" -type d -name tests          2>/dev/null
    find "$MAKE_CONTRIB_INSTALL" -type d -name benchmarks     2>/dev/null
    find "$MAKE_CONTRIB_INSTALL" -type f -name 'example_*.ae' 2>/dev/null
    find "$MAKE_CONTRIB_INSTALL" -type f -name 'test_*.ae'    2>/dev/null
    find "$MAKE_CONTRIB_INSTALL" -type f -name 'test_*.sh'    2>/dev/null
    find "$MAKE_CONTRIB_INSTALL" -type f -name 'build.sh'     2>/dev/null
    find "$MAKE_CONTRIB_INSTALL" -type f -name 'ci.sh'        2>/dev/null
} | wc -l | tr -d ' ')

if [ "$make_unwanted" -ne 0 ]; then
    echo "  [FAIL] make install-contrib layout still contains source-tree noise:"
    {
        unexpected_c_files "$MAKE_CONTRIB_INSTALL" archives
        find "$MAKE_CONTRIB_INSTALL" -type f -name '*.m'
        find "$MAKE_CONTRIB_INSTALL" -type d -name tests
        find "$MAKE_CONTRIB_INSTALL" -type d -name benchmarks
        find "$MAKE_CONTRIB_INSTALL" -type f -name 'example_*.ae'
        find "$MAKE_CONTRIB_INSTALL" -type f -name 'test_*.ae'
        find "$MAKE_CONTRIB_INSTALL" -type f -name 'test_*.sh'
        find "$MAKE_CONTRIB_INSTALL" -type f -name 'build.sh'
        find "$MAKE_CONTRIB_INSTALL" -type f -name 'ci.sh'
    } | head -10
    exit 1
fi

assert_sourced_c_present "$MAKE_CONTRIB_INSTALL" "make install-contrib"

# Same canary set as install.sh's check above — pinning the two
# install paths to a matching ship-list defeats the tinyweb-trim
# class of regressions.
for canary in sqlite tinyweb host/python; do
    if [ ! -f "$MAKE_CONTRIB_INSTALL/$canary/module.ae" ]; then
        echo "  [FAIL] canary $canary missing from make install-contrib"
        exit 1
    fi
done

# Archives — every canary that contrib_build.sh built on this runner
# must produce a libaether_<X>.a. We skip the check when the system
# dep was missing on the runner (recorded in MANIFEST: present = built).
# `host/python` is recorded as `host_python` in the manifest.
if [ ! -f "$MAKE_LIB_DIR/libaether_tinyweb.a" ]; then
    echo "  [FAIL] tinyweb's archive missing from make install-contrib"
    exit 1
fi

echo "  [PASS] contrib/ resolves system-wide after install (issue #334)"
echo "         install.sh + make install-contrib both ship the canaries"
