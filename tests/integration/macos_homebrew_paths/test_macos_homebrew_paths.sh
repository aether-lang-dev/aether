#!/bin/sh
# ae on macOS searches Homebrew's include and lib directories.
#
# Apple's clang searches /usr/local but not /opt/homebrew, where Homebrew lives
# on Apple Silicon, so a module whose C includes a Homebrew-installed header
# (contrib.vulkan: <vulkan/vulkan.h>) failed with "file not found" unless the
# program added -I/opt/homebrew/include itself. ae now adds the prefix --
# HOMEBREW_PREFIX, else /opt/homebrew -- with -idirafter (searched after the
# SDK's and aether's own headers, so a formula can never shadow them) and its
# lib directory last on the link line.
#
# Hermetic: HOMEBREW_PREFIX points at a prefix made here, holding a header, a
# static library, and a decoy <string.h> that #errors -- which must NOT be
# picked up in place of the SDK's.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=macos_homebrew_paths

[ "$(uname -s)" = "Darwin" ] || { echo "  [SKIP] $NAME: macOS only"; exit 0; }
[ -x "$AE" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }
command -v cc >/dev/null 2>&1 || { echo "  [SKIP] $NAME: no cc"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || :' EXIT
fail() { echo "  [FAIL] $NAME: $1"; exit 1; }

BREW="$TMP/brew"
mkdir -p "$BREW/include" "$BREW/lib" "$TMP/proj/lib/probe"
cat > "$BREW/include/aeprobe_brew.h" <<'H'
static inline int aeprobe_hdr_value(void) { return 40; }
int aeprobe_lib_value(void);
H
# A formula that ships a header named like a system one must not win over it.
echo '#error "Homebrew prefix shadowed the SDK string.h"' > "$BREW/include/string.h"
printf 'int aeprobe_lib_value(void) { return 2; }\n' > "$TMP/probe.c"
cc -c "$TMP/probe.c" -o "$TMP/probe.o" && ar rcs "$BREW/lib/libaeprobe.a" "$TMP/probe.o" \
    || fail "could not build the probe library"

cat > "$TMP/proj/lib/probe/module.ae" <<'M'
@c_include("aeprobe_brew.h")
@link("-laeprobe")
exports (value)
extern aeprobe_hdr_value() -> int
extern aeprobe_lib_value() -> int
value() -> int { return aeprobe_hdr_value() + aeprobe_lib_value() }
M
cat > "$TMP/proj/main.ae" <<'M'
import probe
main() { println("probe ${probe.value()}") }
M

cd "$TMP/proj"
HOMEBREW_PREFIX="$BREW" "$AE" build main.ae -o main >"$TMP/build.log" 2>&1 \
    || { sed -n '1,15p' "$TMP/build.log"; fail "a Homebrew header and library were not found"; }
OUT=$(./main 2>&1)
[ "$OUT" = "probe 42" ] || fail "expected 'probe 42', got '$OUT'"

echo "  [PASS] $NAME: Homebrew include + lib found, SDK headers not shadowed"
