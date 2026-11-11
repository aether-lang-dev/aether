#!/bin/sh
# The FreeBSD release ships contrib.sqlite's archives, as Linux's and macOS's
# do: lib/contrib/libaether_sqlite.a and libsqlite3.a, built for FreeBSD.
#
# Until 2026-11 the FreeBSD leg staged its archive without them (a native
# stage builds them for the machine it runs on, and the FreeBSD release is
# cross-built on Linux), so a program linked with `-laether_sqlite -lsqlite3`
# (aeb's link, an aether.toml link_flags list: sae) failed against a FreeBSD
# release with "unable to find library -laether_sqlite". The FreeBSD leg now
# passes `--contrib-target`, which builds them with zig for the target.
#
# Two checks:
#   - release.yml's FreeBSD leg stages with --contrib-target (always run:
#     the release workflow cannot be run anywhere but at a release);
#   - staging with --contrib-target x86_64-freebsd.15.0 gives both archives,
#     each holding x86-64 ELF objects. Needs zig on PATH and a FreeBSD base
#     sysroot in AETHER_SYSROOT (aether-crossbuild's fetch-freebsd-base.sh),
#     as the cross-build does; CI's FreeBSD cross job runs it with both. It
#     skips without them, and offline (the SQLite amalgamation is fetched).
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
NAME=release_cross_contrib_archives

# The FreeBSD leg: the stage-release.sh line between its job header and the
# next job's.
leg=$(sed -n '/^  build-freebsd:/,/^  [a-z][a-z-]*:$/p' "$ROOT/.github/workflows/release.yml" \
      | grep 'scripts/stage-release.sh')
if [ -z "$leg" ]; then
    echo "  [FAIL] $NAME: no stage-release.sh call in release.yml's build-freebsd job"
    exit 1
fi
case "$leg" in
    *--contrib-target*) ;;
    *) echo "  [FAIL] $NAME: release.yml's FreeBSD leg stages without --contrib-target:"
       echo "        $leg"
       exit 1 ;;
esac

if ! command -v zig >/dev/null 2>&1; then
    echo "  [SKIP] $NAME: the cross stage needs zig on PATH (checked the workflow only)"
    exit 0
fi
if [ -z "${AETHER_SYSROOT:-}" ] || [ ! -f "$AETHER_SYSROOT/lib/libc.so.7" ]; then
    echo "  [SKIP] $NAME: the cross stage needs a FreeBSD base sysroot in AETHER_SYSROOT (checked the workflow only)"
    exit 0
fi
command -v file >/dev/null 2>&1 || { echo "  [SKIP] $NAME: no file(1)"; exit 0; }

# What the stage copies as the toolchain does not matter here; the FreeBSD
# cross build's is used when there is one (make ... FREEBSD=1), and checked
# for what a macOS host used to put in it: an `ar` with no index for ELF
# objects left libaether.a empty, and the host's CoreAudio frameworks went
# into ae's link line, so `ae build` failed on FreeBSD either way.
bin="$ROOT/build/.alien/freebsd-x86_64"
if [ -s "$bin/MANIFEST" ]; then
    if ! zig ar t "$bin/libaether.a" 2>/dev/null | grep -q '\.o$'; then
        echo "  [FAIL] $NAME: the FreeBSD cross build's libaether.a holds no objects"
        exit 1
    fi
    if command -v strings >/dev/null 2>&1 && strings "$bin/ae" | grep -q -- '-framework'; then
        echo "  [FAIL] $NAME: the FreeBSD cross build's ae links programs with macOS frameworks"
        exit 1
    fi
else
    bin="$ROOT/build"
fi
[ -s "$bin/MANIFEST" ] || { echo "  [SKIP] $NAME: no build/MANIFEST"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT
rel="$TMP/release"
if ! sh "$ROOT/scripts/stage-release.sh" "$bin" "$rel" --contrib-target x86_64-freebsd.15.0 \
        > "$TMP/stage.log" 2>&1; then
    if grep -q "fetch-sqlite" "$TMP/stage.log"; then
        echo "  [SKIP] $NAME: the SQLite amalgamation could not be fetched (offline?)"
        exit 0
    fi
    echo "  [FAIL] $NAME: scripts/stage-release.sh --contrib-target failed"
    tail -12 "$TMP/stage.log" | sed 's/^/        /'
    exit 1
fi
for a in libaether_sqlite.a libsqlite3.a; do
    [ -f "$rel/lib/contrib/$a" ] || { echo "  [FAIL] $NAME: no lib/contrib/$a"; exit 1; }
    x="$TMP/x.$a"
    mkdir -p "$x"
    (cd "$x" && zig ar x "$rel/lib/contrib/$a") || { echo "  [FAIL] $NAME: cannot unpack $a"; exit 1; }
    for o in "$x"/*.o; do
        d=$(file -b "$o")
        case "$d" in
            "ELF 64-bit LSB relocatable, x86-64"*) ;;
            *) echo "  [FAIL] $NAME: $a holds $(basename "$o"), not an x86-64 ELF object: $d"; exit 1 ;;
        esac
    done
done
echo "  [PASS] $NAME: the FreeBSD stage ships lib/contrib/libaether_sqlite.a and libsqlite3.a, for x86-64 FreeBSD"
