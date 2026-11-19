#!/bin/sh
# #3623: `ae toolchain zig` installs the pinned Zig only from an archive whose
# sha256 is the one compiled into ae (tools/ae_zig_pin.h), and `--check`
# reports an install that is absent or not the pinned one.
#
# Hermetic: AE_ZIG_BASE_URL points at a file:// directory holding a fake
# archive under every pinned archive name, and HOME, USERPROFILE and
# AETHER_HOME at scratch directories. No network is used.
#
# Pinned properties:
#   1. `--check` with nothing installed exits non-zero, says it is not
#      installed, and creates nothing under HOME;
#   2. an archive whose sha256 is not the pinned one is refused: exit
#      non-zero, the pinned hash and the file's hash both printed, and
#      nothing left in the toolchains directory (no install, no staging
#      directory, no archive);
#   3. an install with the stamp `ae toolchain zig` writes and a zig that
#      reports the pinned version passes `--check`, and `ae toolchain zig`
#      uses it without downloading (the mirror would be refused), printing
#      the absolute path of its zig as the last line on stdout;
#   4. the same install with a zig that reports another version fails
#      `--check`;
#   5. the same install with no stamp fails `--check`.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"
[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] ae_toolchain_zig: ae not built"; exit 0; }

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

fail() {
    echo "  [FAIL] ae_toolchain_zig: $1"
    [ -n "${2:-}" ] && [ -f "$2" ] && sed 's/^/    /' "$2" | head -24
    exit 1
}

PIN="$ROOT/tools/ae_zig_pin.h"
VER="$(sed -n 's/^#define AE_ZIG_VERSION "\(.*\)"$/\1/p' "$PIN")"
[ -n "$VER" ] || fail "could not read AE_ZIG_VERSION from $PIN"

# On Windows ae takes native paths; cygpath -m gives D:/... forms.
native() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf '%s\n' "$1"; fi
}

# Fake zig programs, built before AETHER_HOME is set so the build uses this
# tree's toolchain as usual.
mkdir -p "$TMP/src"
printf 'main() {\n    println("%s")\n}\n' "$VER" > "$TMP/src/goodzig.ae"
printf 'main() {\n    println("0.0.1")\n}\n' > "$TMP/src/oldzig.ae"
"$AE" build "$TMP/src/goodzig.ae" -o "$TMP/bin/good/zig" >"$TMP/build.log" 2>&1 \
    || fail "could not build the fake zig" "$TMP/build.log"
"$AE" build "$TMP/src/oldzig.ae" -o "$TMP/bin/old/zig" >"$TMP/build.log" 2>&1 \
    || fail "could not build the old fake zig" "$TMP/build.log"
ZIGEXE=zig
[ -f "$TMP/bin/good/zig.exe" ] && ZIGEXE=zig.exe
[ -f "$TMP/bin/good/$ZIGEXE" ] || fail "the fake zig was not written"

# The mirror: the same bytes under every pinned archive name, so whichever
# host this ae was built for asks for one that is there.
MIRROR="$TMP/mirror/$VER"
mkdir -p "$MIRROR"
printf 'this is not a zig release\n' > "$TMP/fake-archive"
for host in $(sed -n 's/^ *{ "\([a-z0-9_]*-[a-z]*\)", "\([.a-z]*\)", .*/\1\2/p' "$PIN"); do
    ext=".${host#*.}"
    cp "$TMP/fake-archive" "$MIRROR/zig-${host%%.*}-$VER$ext"
done
[ -n "$(ls "$MIRROR")" ] || fail "no pinned hosts read from $PIN"
export AE_ZIG_BASE_URL="file://$(native "$TMP/mirror")"

export HOME="$TMP/home"
USERPROFILE="$(native "$HOME")"
export USERPROFILE
mkdir -p "$HOME"
unset AETHER_HOME

# 1. --check with nothing installed.
"$AE" toolchain zig --check >"$TMP/out" 2>"$TMP/err" && fail "--check passed with nothing installed" "$TMP/err"
grep -q "not installed" "$TMP/err" || fail "--check did not say zig is not installed" "$TMP/err"
[ -z "$(ls -A "$HOME")" ] || fail "--check created files under HOME: $(ls -A "$HOME")"

# 2. A bad hash is refused and nothing is installed.
AH="$TMP/aehome"
mkdir -p "$AH"
AETHER_HOME="$(native "$AH")" "$AE" toolchain zig >"$TMP/out" 2>"$TMP/err" \
    && fail "an archive with the wrong sha256 was accepted" "$TMP/err"
grep -q "sha256 mismatch" "$TMP/err" || fail "the refusal does not name the sha256 mismatch" "$TMP/err"
want="$(sed -n 's/^ *expected \([0-9a-f]\{64\}\)$/\1/p' "$TMP/err")"
got="$(sed -n 's/^ *got *\([0-9a-f]\{64\}\)$/\1/p' "$TMP/err")"
[ -n "$want" ] && [ -n "$got" ] || fail "the refusal does not print both hashes" "$TMP/err"
grep -q "\"$want\"" "$PIN" || fail "the expected hash $want is not one of the pinned ones"
[ "$want" != "$got" ] || fail "the two hashes printed are the same"
if command -v sha256sum >/dev/null 2>&1; then
    actual="$(sha256sum "$TMP/fake-archive" | cut -c1-64)"
elif command -v shasum >/dev/null 2>&1; then
    actual="$(shasum -a 256 "$TMP/fake-archive" | cut -c1-64)"
else
    actual="$got"
fi
[ "$got" = "$actual" ] || fail "the hash printed as got ($got) is not the archive's ($actual)"
[ -s "$TMP/out" ] && fail "a refused install printed a path: $(cat "$TMP/out")"
[ -z "$(ls -A "$AH/toolchains")" ] || fail "a refused install left files: $(ls -A "$AH/toolchains")"

# 3. A good install passes --check and is used without a download.
INST="$AH/toolchains/zig-$VER"
mkdir -p "$INST"
cp "$TMP/bin/good/$ZIGEXE" "$INST/$ZIGEXE"
# The stamp names the archive of the host whose pinned hash ae expected.
row="$(grep "\"$want\"" "$PIN")"
h="$(printf '%s\n' "$row" | sed -n 's/^ *{ "\([^"]*\)", "\([^"]*\)".*/\1/p')"
ext="$(printf '%s\n' "$row" | sed -n 's/^ *{ "\([^"]*\)", "\([^"]*\)".*/\2/p')"
[ -n "$h" ] && [ -n "$ext" ] || fail "could not read the pinned row for $want"
printf 'zig %s zig-%s-%s%s sha256 %s\n' "$VER" "$h" "$VER" "$ext" "$want" > "$INST/.ae-toolchain"

AETHER_HOME="$(native "$AH")" "$AE" toolchain zig --check >"$TMP/out" 2>"$TMP/err" \
    || fail "--check refused a good install" "$TMP/err"
AETHER_HOME="$(native "$AH")" "$AE" toolchain zig >"$TMP/out" 2>"$TMP/err" \
    || fail "ae toolchain zig did not use the good install" "$TMP/err"
grep -q "downloading" "$TMP/err" && fail "ae toolchain zig downloaded with a good install present" "$TMP/err"
last="$(tail -n 1 "$TMP/out" | tr -d '\r')"
expect="$(native "$(cd "$INST" && pwd -P)")/$ZIGEXE"
[ "$last" = "$expect" ] || fail "the last stdout line is '$last', not '$expect'"

# 4. Another version fails --check.
cp "$TMP/bin/old/$ZIGEXE" "$INST/$ZIGEXE"
AETHER_HOME="$(native "$AH")" "$AE" toolchain zig --check >"$TMP/out" 2>"$TMP/err" \
    && fail "--check passed a zig that reports 0.0.1"
grep -q "reports 0.0.1" "$TMP/err" || fail "--check did not name the version it found" "$TMP/err"

# 5. No stamp fails --check.
cp "$TMP/bin/good/$ZIGEXE" "$INST/$ZIGEXE"
rm -f "$INST/.ae-toolchain"
AETHER_HOME="$(native "$AH")" "$AE" toolchain zig --check >"$TMP/out" 2>"$TMP/err" \
    && fail "--check passed an install with no stamp"
grep -q "is missing" "$TMP/err" || fail "--check did not say the stamp is missing" "$TMP/err"

echo "  [PASS] ae_toolchain_zig"
