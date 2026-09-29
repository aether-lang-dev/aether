#!/bin/sh
# #2208: a project that gets Aether from a gh-release binary tarball must be
# able to `import contrib.<X>`. The release tarball shipped share/aether/
# runtime and std but not contrib, so `import contrib.jq` on a binary
# install failed to resolve — while a source install (install.sh copies
# contrib/) worked. release.yml now copies + trims contrib/ into the
# tarball, the same way install.sh does.
#
# This test reproduces the tarball LAYOUT release.yml produces (bin +
# share/aether/{runtime,std,contrib}, trimmed) without running the workflow,
# points AETHER_HOME at it, and builds a program that imports contrib. It
# checks a pure-Aether module (png) and one whose C is @source'd from a
# sibling .ae (jq's value.ae names aether_jq.c), so both the ".ae ships" and
# "the @source'd .c ships and recompiles" halves are exercised.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
# On the MSYS2 Windows leg the binaries are ae.exe / aetherc.exe. The sweep
# passes EXE_EXT; fall back to detecting the suffix from what is on disk so
# the test does not depend on that being exported.
EXE="${EXE_EXT:-}"
if [ -z "$EXE" ] && [ ! -x "$ROOT/build/ae" ] && [ -x "$ROOT/build/ae.exe" ]; then
    EXE=".exe"
fi
AE="$ROOT/build/ae$EXE"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] release_contrib_resolves: build/ae not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT
rel="$TMPDIR/release"
mkdir -p "$rel/bin" "$rel/share/aether" "$rel/include/aether"

cp "$ROOT/build/aetherc$EXE" "$ROOT/build/ae$EXE" "$rel/bin/"
cp -r "$ROOT/runtime" "$ROOT/std" "$rel/share/aether/"
[ -f "$ROOT/build/MANIFEST" ] && cp "$ROOT/build/MANIFEST" "$rel/share/aether/"
cp "$ROOT/include"/*.h "$rel/include/aether/" 2>/dev/null || true

# The step release.yml adds: copy contrib, drop the source-tree noise, then
# trim the .c/.m down to what a module still compiles from (same recipe).
cp -r "$ROOT/contrib" "$rel/share/aether/"
find "$rel/share/aether/contrib" -type d -name tests      -exec rm -rf {} + 2>/dev/null || true
find "$rel/share/aether/contrib" -type d -name benchmarks -exec rm -rf {} + 2>/dev/null || true
find "$rel/share/aether/contrib" -type f -name 'example_*.ae' -delete 2>/dev/null || true
find "$rel/share/aether/contrib" -type f -name 'test_*.ae' -delete 2>/dev/null || true
find "$rel/share/aether/contrib" -type f -name 'test_*.sh' -delete 2>/dev/null || true
find "$rel/share/aether/contrib" -type f -name 'build.sh'  -delete 2>/dev/null || true
find "$rel/share/aether/contrib" -type f -name 'ci.sh'     -delete 2>/dev/null || true
sh "$ROOT/.github/scripts/trim_contrib_sources.sh" "$rel/share/aether/contrib" --keep-host-bridges 2>/dev/null || true

# A pure-Aether module resolves and builds.
cat > "$TMPDIR/png_consumer.ae" <<'AE'
import contrib.png
main() { println("png import ok") }
AE
if ! ( cd "$TMPDIR" && AETHER_HOME="$rel" "$rel/bin/ae$EXE" build png_consumer.ae -o "$TMPDIR/png" ) \
        > "$TMPDIR/png.log" 2>&1; then
    echo "  [FAIL] release_contrib_resolves: contrib.png did not build against the release layout"
    tail -8 "$TMPDIR/png.log" | sed 's/^/        /'
    exit 1
fi

# jq @sources aether_jq.c from value.ae — build AND run, so the trimmed .c
# both shipped and recompiled into the program.
cat > "$TMPDIR/jq_consumer.ae" <<'AE'
import std.json
import contrib.jq
main() {
    out, err = jq.query(".a[1]", "{\"a\": [10, 20, 30]}")
    if err != "" { println("ERR: ${err}") } else { println("jq=${out}") }
}
AE
if ! ( cd "$TMPDIR" && AETHER_HOME="$rel" "$rel/bin/ae$EXE" build jq_consumer.ae -o "$TMPDIR/jq" ) \
        > "$TMPDIR/jq.log" 2>&1; then
    echo "  [FAIL] release_contrib_resolves: contrib.jq did not build against the release layout"
    tail -8 "$TMPDIR/jq.log" | sed 's/^/        /'
    exit 1
fi
got="$("$TMPDIR/jq$EXE" 2>&1 || true)"
if [ "$got" != "jq=20" ]; then
    echo "  [FAIL] release_contrib_resolves: contrib.jq ran wrong: expected 'jq=20', got '$got'"
    exit 1
fi

echo "  [PASS] release_contrib_resolves: contrib.png and contrib.jq build (and jq runs) against a release tarball layout"
exit 0
