#!/bin/sh
# `ae build --target` links contrib.xmp with no libxmp on the target,
# compiling the pinned libxmp source once per target and reusing it.
#
# For each musl target (static, so it runs without a target sysroot):
#   - contrib/xmp/test_xmp.ae, the module's full spec, cross-builds and passes
#     when run (natively for the host's arch, under qemu-user for another
#     arch when it is installed; otherwise the binary's arch is checked and
#     running is skipped)
#   - the binary is statically linked: nothing for a libxmp to satisfy
#   - libxmp's objects sit in the ae cache under the target's name, and a
#     second build reuses them ("Compiling libxmp" is printed on a miss only)
#
# Needs zig on PATH and the fetched libxmp source; skips without either.
# Uses $AETHER_CACHE_DIR when set (CI persists it), a fresh one otherwise.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/portable.sh"
AE="$ROOT/build/ae"
NAME=xmp_vendored_cross
[ -x "$AE" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }
command -v zig >/dev/null 2>&1 || { echo "  [SKIP] $NAME: zig not on PATH (scripts/get-zig.sh)"; exit 0; }
[ -f "$ROOT/contrib/xmp/libxmp/sources.txt" ] || {
    echo "  [SKIP] $NAME: no libxmp source (scripts/fetch-libxmp.sh)"; exit 0; }

T="$(mktemp -d)"
trap 'rm -rf "$T" || true' EXIT
if [ -z "${AETHER_CACHE_DIR:-}" ]; then
    AETHER_CACHE_DIR="$T/cache"
fi
export AETHER_CACHE_DIR
mkdir -p "$AETHER_CACHE_DIR"

TARGETS="${XMP_CROSS_TARGETS:-x86_64-linux-musl aarch64-linux-musl}"
HOST_ARCH="$(uname -m)"
[ "$HOST_ARCH" = arm64 ] && HOST_ARCH=aarch64

fail() {
    echo "  [FAIL] $NAME: $1"
    [ -n "${2:-}" ] && printf '%s\n' "$2" | grep -vE '^[[:space:]]*$' | tail -12 | sed 's/^/    /'
    exit 1
}

cat > "$T/info.ae" <<'AE'
import contrib.xmp

main() {
    p, err = xmp.load("", 0)
    if p == null { println("refused: ${err}") }
}
AE
cp "$ROOT/contrib/xmp/test_xmp.ae" "$T/spec.ae"

# A musl Linux binary runs natively only on Linux.
runner_for() {
    [ "$(uname -s)" = Linux ] || return 1
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
    ls "$AETHER_CACHE_DIR" | grep -q "^libxmp[0-9]*-$target-.*\.o$" && cold=0
    FIRST=$(cd "$T" && "$AE" build --target="$target" info.ae -o "info-$target" 2>&1) || {
        case "$FIRST" in
            *"sub-compilation of musl libc.a failed"*)
                echo "  [SKIP] $NAME: zig cannot build musl libc.a on this host"; exit 0 ;;
        esac
        fail "info.ae did not build for $target" "$FIRST"; }
    if [ "$cold" = 1 ]; then
        case "$FIRST" in *"Compiling libxmp for $target"*) ;;
            *) fail "a cold cache did not compile libxmp for $target" "$FIRST" ;; esac
    fi
    n=$(ls "$AETHER_CACHE_DIR" | grep -c "^libxmp[0-9]*-$target-.*\.o$" || true)
    want_n=$(grep -c . "$ROOT/contrib/xmp/libxmp/sources.txt")
    [ "$n" -ge "$want_n" ] || fail "$n of libxmp's $want_n objects are cached for $target"

    SPEC_OUT=$(cd "$T" && "$AE" build --target="$target" spec.ae -o "spec-$target" 2>&1) \
        || fail "the contrib.xmp spec did not build for $target" "$SPEC_OUT"
    case "$SPEC_OUT" in *"Compiling libxmp"*) fail "a warm cache recompiled libxmp for $target" "$SPEC_OUT" ;; esac

    if command -v file >/dev/null 2>&1; then
        desc="$(file_desc "$T/spec-$target")"
        printf '%s' "$desc" | grep -q "$want_file" || fail "$target binary has the wrong arch" "$desc"
        printf '%s' "$desc" | grep -q "statically linked" || fail "$target binary is not static" "$desc"
    fi

    if run=$(runner_for "$arch"); then
        GOT=$(cd "$T" && $run "./info-$target") || fail "$target probe did not run" "$GOT"
        case "$GOT" in "refused: xmp: "*) ;; *) fail "$target probe printed '$GOT'" ;; esac
        SPEC=$(cd "$T" && $run "./spec-$target" 2>&1) || fail "the contrib.xmp spec failed on $target" "$SPEC"
        printf '%s' "$SPEC" | grep -q "passing" || fail "the spec reported no passes on $target" "$SPEC"
        printf '%s' "$SPEC" | grep -qiE "failing|pending|skipped" && fail "the spec did not pass outright on $target" "$SPEC"
        echo "    $target: built, ran the spec${run:+ (under $run)}"
    else
        echo "    $target: built (no qemu-$arch here, so not run)"
    fi
done

echo "  [PASS] $NAME: self-contained libxmp for $TARGETS"
