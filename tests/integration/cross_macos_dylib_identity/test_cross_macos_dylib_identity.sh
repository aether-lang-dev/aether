#!/bin/sh
# A macOS `--emit=lib` cross build carries a loadable identity, whatever host
# ran it.
#
# zig spells the macOS targets `aarch64-macos-none` / `x86_64-macos-none`, with
# no "-apple-" in them, and the cross link took only "-apple-" for an Apple
# target. A macOS dylib linked on Linux or FreeBSD therefore went through the
# ELF arm: no -install_name, so zig recorded the -o path as LC_ID_DYLIB (a
# release built on CI said /home/runner/work/.../libfoo.so, and every program
# linked against it failed with "Library not loaded"), and no deployment
# target, so the minimum was zig's default (15.0) rather than one ae chose.
# A Mac host rewrote the id afterwards with install_name_tool, which hid the
# first half there.
#
# Asserts, reading the load commands directly (no otool on a Linux runner):
#   - the artifact is a Mach-O dylib, its LC_ID_DYLIB @rpath/<leaf>;
#   - its minos is ae's documented default (13.0), and AETHER_MACOS_MIN
#     overrides it;
#   - --size links with the Mach-O strip spellings (x86_64 build);
#   - on a macOS host of the same architecture, a C program linked -lgreet
#     loads the library by that id and calls into it.
#
# Skips without zig or a host C compiler. Cost: two cross links, each
# compiling the runtime and stdlib for its target.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=cross_macos_dylib_identity
HOSTCC="${CC:-cc}"

if ! command -v zig >/dev/null 2>&1; then
    echo "  [SKIP] $NAME: zig not on PATH"
    exit 0
fi
if ! command -v "$HOSTCC" >/dev/null 2>&1; then
    echo "  [SKIP] $NAME: no host C compiler ($HOSTCC)"
    exit 0
fi

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT INT TERM

fail() { echo "  [FAIL] $NAME: $*"; exit 1; }

"$HOSTCC" -O2 -o "$T/macho_id" "$SCRIPT_DIR/macho_id.c" || fail "could not build macho_id"

# check <dylib> <want-minos>
check() {
    "$T/macho_id" "$1" > "$T/id.txt" || fail "$1 is not a 64-bit Mach-O"
    grep -qx 'type=dylib' "$T/id.txt" || fail "$1 is not a dylib: $(cat "$T/id.txt")"
    want_id="@rpath/$(basename "$1")"
    got_id="$(sed -n 's/^id=//p' "$T/id.txt")"
    [ "$got_id" = "$want_id" ] || fail "$1: LC_ID_DYLIB is '$got_id', want '$want_id'"
    got_min="$(sed -n 's/^minos=//p' "$T/id.txt")"
    [ "$got_min" = "$2" ] || fail "$1: minos is '$got_min', want '$2'"
}

# Deep build directory, so a recorded -o path is unmistakable.
mkdir -p "$T/build/out/arm64" "$T/build/out/x86_64"

if ! "$AE" build --target=aarch64-macos --emit=lib "$SCRIPT_DIR/mylib.ae" \
        -o "$T/build/out/arm64/libgreet.dylib" > "$T/arm64.log" 2>&1; then
    sed 's/^/    /' "$T/arm64.log" | tail -15
    fail "aarch64-macos --emit=lib did not build"
fi
check "$T/build/out/arm64/libgreet.dylib" 13.0

if ! AETHER_MACOS_MIN=14.2 "$AE" build --size --target=x86_64-macos --emit=lib \
        "$SCRIPT_DIR/mylib.ae" -o "$T/build/out/x86_64/libgreet.dylib" > "$T/x86_64.log" 2>&1; then
    sed 's/^/    /' "$T/x86_64.log" | tail -15
    fail "x86_64-macos --size --emit=lib did not build"
fi
check "$T/build/out/x86_64/libgreet.dylib" 14.2

# A consumer resolves the library through the id: only where it can run.
if [ "$(uname -s)" = Darwin ]; then
    case "$(uname -m)" in
        arm64)  dir="$T/build/out/arm64" ;;
        x86_64) dir="$T/build/out/x86_64" ;;
        *)      dir="" ;;
    esac
    if [ -n "$dir" ]; then
        "$HOSTCC" -o "$T/consumer" "$SCRIPT_DIR/consumer.c" -L"$dir" -lgreet \
            -Wl,-rpath,"$dir" || fail "consumer did not link against libgreet.dylib"
        "$T/consumer" || fail "consumer did not load libgreet.dylib or got the wrong answer"
    fi
fi

echo "  [PASS] $NAME"
