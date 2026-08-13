#!/bin/sh
# Regression (#1549): `ae build` HONOURS the `@link` directive, and a
# `-D`-dropped import takes its link flags with it.
#
# The sibling test link_directive/ asserts that aetherc EMITS the
# `// aether-link:` header (union across the closure, dedup, absent when
# unused). It stops at the generated C. This one asserts the other half:
# that `ae build` reads that header back and puts the flags on the link
# command. Before #1549 the header was computed correctly and ignored.
#
# Two cases, and the second is the one a static `link_flags` cannot make:
#
#   1. ENABLED  — a workspace with NO `link_flags` and NO `extra_sources`
#      links libsqlite3 anyway, purely from contrib/sqlite's @link.
#   2. DISABLED — the same source without the build symbol drops the import,
#      so the module leaves the closure and its @link contributes nothing.
#      Asserted with ldd/otool: a binary that merely does not CALL sqlite is
#      not the same as one that does not LINK it.
#
# SQLite isn't auto-detected by the Aether toolchain, so probe and SKIP when
# absent, matching CONTRIBUTING.md §"Coding for portability" pattern #2.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

# -- Capability probe -----------------------------------------------------
probe_sqlite_available() {
    if pkg-config --exists sqlite3 2>/dev/null; then
        return 0
    fi
    probe_c="$TMPDIR/probe_sqlite.c"
    cat > "$probe_c" <<'EOF'
#include <sqlite3.h>
int main(void) { sqlite3* db; return sqlite3_open(0, &db); }
EOF
    gcc "$probe_c" -lsqlite3 -o "$TMPDIR/probe_sqlite" >/dev/null 2>&1
}

if ! probe_sqlite_available; then
    echo "  [PASS] link_directive_build: SKIP (libsqlite3 not installed)"
    exit 0
fi

# The veneer archive is what @link's -laether_sqlite names. `make contrib`
# builds it; without it there is nothing for this test to prove.
if [ ! -f "$ROOT/build/contrib/libaether_sqlite.a" ]; then
    echo "  [PASS] link_directive_build: SKIP (build/contrib/libaether_sqlite.a absent; run \`make contrib\`)"
    exit 0
fi

# -- Stage a workspace with NO link_flags and NO extra_sources ------------
WORK="$TMPDIR/work"
mkdir -p "$WORK"
ln -s "$ROOT/contrib" "$WORK/contrib"
cp "$SCRIPT_DIR/probe.ae" "$WORK/probe.ae"

# Deliberately minimal. Both halves of `@link("-laether_sqlite -lsqlite3")`
# do the work: the veneer archive supplies the C that sibling sqlite tests
# compile via extra_sources, and -lsqlite3 supplies the library.
#
# extra_sources is avoided on purpose. It compiles unconditionally, so a
# workspace listing aether_sqlite.c fails to LINK the disabled build — the
# object needs sqlite3_* symbols that @link correctly no longer requests.
# That asymmetry is pre-existing (a static extra_sources cannot track a
# `when defined` region any more than a static link_flags can) and is not
# what this test is about.
cat > "$WORK/aether.toml" <<'EOF'
[project]
name = "link_directive_build_probe"
version = "0.0.0"

[[bin]]
name = "probe"
path = "probe.ae"
EOF

fail() {
    echo "  [FAIL] link_directive_build: $1"
    shift
    for f in "$@"; do
        [ -f "$f" ] && sed 's/^/    /' "$f" | head -20
    done
    exit 1
}

# -- 1. Enabled: @link alone must satisfy the linker -----------------------
if ! ( cd "$WORK" && "$ROOT/build/ae" build "probe.ae" -D WITH_SQLITE \
        -o "$TMPDIR/on" >"$TMPDIR/build_on.log" 2>&1 ); then
    fail "enabled build failed (@link did not reach the linker)" "$TMPDIR/build_on.log"
fi

# `ae build` can exit 0 even when the gcc link fails, so check for the binary.
[ -x "$TMPDIR/on" ] || fail "enabled build produced no binary" "$TMPDIR/build_on.log"

if ! "$TMPDIR/on" >"$TMPDIR/run_on.log" 2>&1; then
    fail "enabled probe exited non-zero" "$TMPDIR/run_on.log"
fi

if ! grep -q "linked via @link" "$TMPDIR/run_on.log"; then
    fail "enabled probe did not reach the sqlite path" "$TMPDIR/run_on.log"
fi

# -- 2. Disabled: the dropped import takes its link flags with it ----------
if ! ( cd "$WORK" && "$ROOT/build/ae" build "probe.ae" \
        -o "$TMPDIR/off" >"$TMPDIR/build_off.log" 2>&1 ); then
    fail "disabled build failed" "$TMPDIR/build_off.log"
fi

[ -x "$TMPDIR/off" ] || fail "disabled build produced no binary" "$TMPDIR/build_off.log"

if ! "$TMPDIR/off" >"$TMPDIR/run_off.log" 2>&1; then
    fail "disabled probe exited non-zero" "$TMPDIR/run_off.log"
fi

if ! grep -q "sqlite excluded" "$TMPDIR/run_off.log"; then
    fail "disabled probe took the wrong branch" "$TMPDIR/run_off.log"
fi

# The load-bearing assertion: absence, not just non-use. Skip on platforms
# without a dependency lister rather than assert something weaker.
if command -v ldd >/dev/null 2>&1; then
    deps_off=$(ldd "$TMPDIR/off" 2>/dev/null)
    deps_on=$(ldd "$TMPDIR/on" 2>/dev/null)
elif command -v otool >/dev/null 2>&1; then
    deps_off=$(otool -L "$TMPDIR/off" 2>/dev/null)
    deps_on=$(otool -L "$TMPDIR/on" 2>/dev/null)
else
    deps_off=""
    deps_on=""
fi

if [ -n "$deps_on" ]; then
    # Guard against a vacuous pass: if the ENABLED build doesn't show the
    # library either, the check proves nothing about the disabled one.
    if ! printf '%s' "$deps_on" | grep -qi sqlite; then
        echo "  [PASS] link_directive_build: 2 cases (dependency check skipped:" \
             "enabled build links libsqlite3 statically)"
        exit 0
    fi
    if printf '%s' "$deps_off" | grep -qi sqlite; then
        echo "  [FAIL] link_directive_build: disabled build still links libsqlite3"
        printf '%s\n' "$deps_off" | grep -i sqlite | sed 's/^/    /'
        exit 1
    fi
fi

echo "  [PASS] link_directive_build: 2 cases"
