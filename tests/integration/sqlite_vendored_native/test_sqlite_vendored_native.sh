#!/bin/sh
# #1372: a program importing contrib.sqlite links the pinned amalgamation
# `make contrib` built into build/contrib/libsqlite3.a, not the system library.
#
# The module's @link stays `-laether_sqlite -lsqlite3 -lm`; -lsqlite3 finds the
# vendored archive because the -L for build/contrib is searched before the
# system directories. If that ever stopped holding, the program would quietly
# link the system SQLite instead -- a different version, and a runtime
# dependency the issue set out to remove -- so this checks both.
#
# Skips unless `make contrib` produced the vendored archive (it falls back to
# the system library when the amalgamation cannot be fetched). CI's contrib
# job runs it after building one.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] sqlite_vendored_native: ae not built"; exit 0; }
if [ ! -f "$ROOT/build/contrib/libsqlite3.a" ] || [ ! -f "$ROOT/build/contrib/libaether_sqlite.a" ]; then
    echo "  [SKIP] sqlite_vendored_native: no vendored build/contrib/libsqlite3.a (run make contrib)"
    exit 0
fi

WANT="$(sed -n 's/^SQLITE_VERSION=//p' "$ROOT/contrib/sqlite/amalgamation.lock")"
T="$(mktemp -d)"
trap 'rm -rf "$T" || true' EXIT

cat > "$T/version.ae" <<'AE'
import contrib.sqlite

main() {
    db, _err = sqlite.open(":memory:")
    rs, _qerr = sqlite.query(db, "SELECT sqlite_version()")
    println("sqlite ${sqlite.cell(rs, 0, 0)}")
    sqlite.free(rs)
    sqlite.close(db)
}
AE

if ! BUILD=$(AETHER_HOME="$ROOT" "$AE" build "$T/version.ae" -o "$T/version" 2>&1); then
    echo "  [FAIL] sqlite_vendored_native: build failed"
    printf '%s\n' "$BUILD" | grep -iE "error|undefined" | head -8 | sed 's/^/    /'
    exit 1
fi
OUT=$("$T/version")
if [ "$OUT" != "sqlite $WANT" ]; then
    echo "  [FAIL] sqlite_vendored_native: expected 'sqlite $WANT' (the pinned amalgamation), got '$OUT'"
    exit 1
fi

# No runtime dependency on a system libsqlite3.
DEPS=""
if command -v ldd >/dev/null 2>&1; then DEPS=$(ldd "$T/version" 2>&1 || true)
elif command -v otool >/dev/null 2>&1; then DEPS=$(otool -L "$T/version" 2>&1 || true)
fi
if printf '%s' "$DEPS" | grep -qi "libsqlite3"; then
    echo "  [FAIL] sqlite_vendored_native: the program still depends on a system libsqlite3"
    printf '%s\n' "$DEPS" | grep -i sqlite | sed 's/^/    /'
    exit 1
fi

# The static amalgamation needs libm, which its own @link names after
# -lsqlite3. Codegen dedupes link tokens first-seen, so a module that names
# -lm earlier (std.audio's -lpthread -ldl -lm) moves it AHEAD of -lsqlite3;
# ae must still put it after the module archives or the link fails on sqrt
# and friends. SQL's sqrt() makes the math functions load-bearing.
cat > "$T/order.ae" <<'AE'
import std.audio
import contrib.sqlite

main() {
    db, _err = sqlite.open(":memory:")
    rs, _qerr = sqlite.query(db, "SELECT round(sqrt(2.0), 3)")
    println("sqrt ${sqlite.cell(rs, 0, 0)}")
    sqlite.free(rs)
    sqlite.close(db)
}
AE
if ! BUILD=$(AETHER_HOME="$ROOT" "$AE" build "$T/order.ae" -o "$T/order" 2>&1); then
    echo "  [FAIL] sqlite_vendored_native: with std.audio imported first, the link failed"
    printf '%s\n' "$BUILD" | grep -iE "error|undefined" | head -8 | sed 's/^/    /'
    exit 1
fi
OUT=$("$T/order")
[ "$OUT" = "sqrt 1.414" ] || { echo "  [FAIL] sqlite_vendored_native: sqrt() gave '$OUT'"; exit 1; }

echo "  [PASS] sqlite_vendored_native: SQLite $WANT linked from the pinned amalgamation (issue #1372)"
