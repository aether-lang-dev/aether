#!/bin/sh
# Regression (#959): `ae build` must link against a prebuilt runtime archive
# shipped FLAT at lib/libaether.a, not only the canonical nested
# lib/aether/libaether.a. The macOS arm64 v0.331/0.332 packages shipped it
# flat; `ae build` looked only at the nested path, missed it, and fell back to
# compiling an INCOMPLETE runtime source list — so every build (even
# hello-world) failed to link with "Undefined symbols ... _aether_io_poller_init".
#
# Strategy: do a real install to a temp prefix (which lays the archive out
# nested), then relocate it to the flat path to mimic the package layout, and
# confirm `ae build` finds the flat archive (not the source fallback) and links.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

if [ ! -x "$ROOT/build/ae" ] || [ ! -f "$ROOT/install.sh" ]; then
    echo "  [SKIP] flat_lib_fallback: ae/install.sh not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
prefix="$tmpdir/inst"

# The sweep has built the tree: install its binaries without rebuilding them,
# as install_contrib_resolves does. Otherwise install.sh ran `make` four
# times over the shared tree (relinking build/ae beside parallel drivers,
# #2142), built the language server this test never runs and fetched git
# tags from the network, every run (#2596).
if ! AETHER_INSTALL_NO_BUILD=1 "$ROOT/install.sh" "$prefix" < /dev/null > "$tmpdir/install.log" 2>&1; then
    echo "  [SKIP] flat_lib_fallback: install.sh failed (cannot set up fixture)"
    sed 's/^/        /' "$tmpdir/install.log" | head -8
    exit 0
fi

nested="$prefix/lib/aether/libaether.a"
if [ ! -f "$nested" ]; then
    echo "  [SKIP] flat_lib_fallback: install produced no prebuilt archive to relocate"
    exit 0
fi

# Mimic the macOS package: archive flat at lib/, none nested under lib/aether/.
cp "$nested" "$prefix/lib/libaether.a"
rm -f "$nested"

printf 'main() { println("flat-lib ok") }' > "$tmpdir/hello.ae"
AETHER_HOME="$prefix" "$prefix/bin/ae" build "$tmpdir/hello.ae" -o "$tmpdir/hello" \
    --verbose > "$tmpdir/build.log" 2>&1
build_rc=$?

fail=0
if [ "$build_rc" -ne 0 ] || [ ! -x "$tmpdir/hello" ]; then
    echo "  [FAIL] flat_lib_fallback: ae build did not link against the flat lib/libaether.a"
    sed 's/^/        /' "$tmpdir/build.log" | tail -15
    fail=1
elif ! "$tmpdir/hello" 2>/dev/null | grep -q "flat-lib ok"; then
    echo "  [FAIL] flat_lib_fallback: built binary did not run correctly"
    fail=1
fi

# It must have resolved the flat archive, not silently used the (incomplete)
# source fallback.
if grep -q "source fallback" "$tmpdir/build.log"; then
    echo "  [FAIL] flat_lib_fallback: ae fell back to source compile instead of using lib/libaether.a"
    grep -i 'toolchain.*lib' "$tmpdir/build.log" | sed 's/^/        /'
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "  [PASS] flat_lib_fallback: ae build links the flat lib/libaether.a (no incomplete source fallback)"
fi
exit $fail
