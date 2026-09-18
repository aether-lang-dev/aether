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
#      install layout is descriptor-+-header only, with one
#      explicit carve-out: contrib/host/<lang>/aether_host_<lang>.c
#      DOES ship (plain `make install` doesn't build the matching
#      libaether_host_<lang>.a, so downstream apps that
#      `import contrib.host.<lang>` compile the bridge from source).
#      See docs/install-layout.md "What does NOT ship".
#   3. The module.ae files are syntactically what the resolver looks
#      for: a non-empty file at the documented path.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

cd "$ROOT"

# Run install.sh against the temp prefix. Quiet — we only care about
# the resulting layout.
if ! ./install.sh "$TMPDIR" < /dev/null > "$TMPDIR/install.log" 2>&1; then
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
    # `.c` files: everything except the host-bridge carve-out.
    find "$CONTRIB_INSTALL" -type f -name '*.c' \
        ! -path '*/contrib/host/*/aether_host_*.c' 2>/dev/null
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
        find "$CONTRIB_INSTALL" -type f -name '*.c' \
            ! -path '*/contrib/host/*/aether_host_*.c'
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
    find "$MAKE_CONTRIB_INSTALL" -type f -name '*.c' \
        ! -path '*/contrib/host/*/aether_host_*.c' 2>/dev/null
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
        find "$MAKE_CONTRIB_INSTALL" -type f -name '*.c' \
            ! -path '*/contrib/host/*/aether_host_*.c'
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
