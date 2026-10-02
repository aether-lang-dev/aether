#!/bin/sh
# #1372: `ae build --target` links a working SQLite with no target-system
# libsqlite3, compiling the pinned amalgamation once per target and reusing it.
#
# Before this, a cross build of a program importing contrib.sqlite failed at
# the link (sqlite_* undefined) unless a CROSSBUILD_SYSROOT staged both
# libaether_sqlite.a and libsqlite3.a for that target.
#
# For each musl target (static, so it runs without a target sysroot):
#   - contrib/sqlite/test_sqlite.ae, the module's full spec, cross-builds and
#     passes when run (natively for the host's arch, under qemu-user for
#     another arch when it is installed; otherwise the binary's arch is
#     checked and running is skipped)
#   - a probe reports the PINNED version, so it is the amalgamation linked
#   - the binary is statically linked: nothing for a libsqlite3 to satisfy
#   - the amalgamation object sits in the ae cache under the target's name,
#     and a second build reuses it ("Compiling SQLite" is printed on a miss
#     only)
#
# Needs zig on PATH and the fetched amalgamation; skips without either.
# Uses $AETHER_CACHE_DIR when set (CI persists it), a fresh one otherwise.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] sqlite_vendored_cross: ae not built"; exit 0; }
command -v zig >/dev/null 2>&1 || { echo "  [SKIP] sqlite_vendored_cross: zig not on PATH (scripts/get-zig.sh)"; exit 0; }
[ -f "$ROOT/contrib/sqlite/amalgamation/sqlite3.c" ] || {
    echo "  [SKIP] sqlite_vendored_cross: no amalgamation (scripts/fetch-sqlite-amalgamation.sh)"; exit 0; }

T="$(mktemp -d)"
trap 'rm -rf "$T" || true' EXIT
if [ -z "${AETHER_CACHE_DIR:-}" ]; then
    AETHER_CACHE_DIR="$T/cache"
fi
export AETHER_CACHE_DIR
mkdir -p "$AETHER_CACHE_DIR"

WANT="$(sed -n 's/^SQLITE_VERSION=//p' "$ROOT/contrib/sqlite/amalgamation.lock")"
TARGETS="${SQLITE_CROSS_TARGETS:-x86_64-linux-musl aarch64-linux-musl}"
HOST_ARCH="$(uname -m)"

fail() {
    echo "  [FAIL] sqlite_vendored_cross: $1"
    [ -n "${2:-}" ] && printf '%s\n' "$2" | grep -vE '^\s*$' | tail -12 | sed 's/^/    /'
    exit 1
}

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
# The spec, beside a contrib/ link so its `import contrib.sqlite` resolves
# the same way from the scratch directory.
cp "$ROOT/contrib/sqlite/test_sqlite.ae" "$T/spec.ae"

# run_on ARCH BIN -- how to execute a binary for ARCH here, or "" if we can't.
runner_for() {
    case "$1" in
        "$HOST_ARCH") echo "" ; return 0 ;;
    esac
    for q in "qemu-$1-static" "qemu-$1"; do
        if command -v "$q" >/dev/null 2>&1; then echo "$q"; return 0; fi
    done
    return 1
}

for target in $TARGETS; do
    arch="${target%%-*}"
    case "$arch" in
        x86_64)  want_file="x86-64" ;;
        aarch64) want_file="aarch64" ;;
        *)       want_file="$arch" ;;
    esac

    cold=1
    ls "$AETHER_CACHE_DIR" | grep -q "^sqlite3-$target-.*\.o$" && cold=0
    FIRST=$(cd "$T" && "$AE" build --target="$target" version.ae -o "version-$target" 2>&1) \
        || fail "version.ae did not build for $target" "$FIRST"
    if [ "$cold" = 1 ]; then
        case "$FIRST" in *"Compiling SQLite for $target"*) ;;
            *) fail "a cold cache did not compile SQLite for $target" "$FIRST" ;; esac
    fi
    slot=$(ls "$AETHER_CACHE_DIR" | grep "^sqlite3-$target-.*\.o$" | head -1 || true)
    [ -n "$slot" ] || fail "no cached amalgamation object for $target in $AETHER_CACHE_DIR"

    # Every build after the first reuses the object.
    SPEC_OUT=$(cd "$T" && "$AE" build --target="$target" spec.ae -o "spec-$target" 2>&1) \
        || fail "the contrib.sqlite spec did not build for $target" "$SPEC_OUT"
    case "$SPEC_OUT" in *"Compiling SQLite"*) fail "a warm cache recompiled SQLite for $target" "$SPEC_OUT" ;; esac

    if command -v file >/dev/null 2>&1; then
        desc="$(file -b "$T/version-$target")"
        printf '%s' "$desc" | grep -q "$want_file" || fail "$target binary has the wrong arch" "$desc"
        printf '%s' "$desc" | grep -q "statically linked" || fail "$target binary is not static" "$desc"
    fi

    if run=$(runner_for "$arch"); then
        GOT=$(cd "$T" && $run "./version-$target") || fail "$target probe did not run" "$GOT"
        [ "$GOT" = "sqlite $WANT" ] || fail "$target probe reported '$GOT', want 'sqlite $WANT'"
        SPEC=$(cd "$T" && $run "./spec-$target" 2>&1) || fail "the contrib.sqlite spec failed on $target" "$SPEC"
        # it_when skips every case when SQLite cannot open, which would exit 0
        # having tested nothing. Require passes and no skips.
        printf '%s' "$SPEC" | grep -q "passing" || fail "the spec reported no passes on $target" "$SPEC"
        printf '%s' "$SPEC" | grep -qiE "pending|skipped" && fail "the spec skipped cases on $target" "$SPEC"
        echo "    $target: built, ran the spec, SQLite $WANT${run:+ (under $run)}"
    else
        echo "    $target: built (no qemu-$arch here, so not run)"
    fi
done

echo "  [PASS] sqlite_vendored_cross: self-contained SQLite for $TARGETS (issue #1372)"
