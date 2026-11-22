#!/bin/sh
# #3810: `ae` asks the C compiler whether it takes a flag by compiling a probe
# with -fsyntax-only. `zig cc` adds -c and an output path of its own to every
# clang command and reads the object back, so under -fsyntax-only, where
# clang writes none, every probe failed with "FileNotFound", flag or not. The
# failure read as a refusal: with AE_CC="zig cc", `ae` never gave Clang
# -fbracket-depth (#3574), and a program nested past Clang's default 256
# brackets did not compile. A probe whose control compile fails too now
# tries the next form (-c into an object) instead of concluding "refused".
#
# Also (#3814): the cache key names zig's clang version.
#
# Needs zig: on PATH, or unpacked under build/scratch/.zig of this tree or of
# the main checkout it is a worktree of. Skips without it.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=zig_cc_probe

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] $NAME: ae not built"
    exit 0
fi

. "$ROOT/tests/lib/find_zig.sh"
zig="$(find_zig "$ROOT")"
if [ -z "$zig" ]; then
    echo "  [SKIP] $NAME: no zig on PATH or under build/scratch/.zig"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
AETHER_CACHE_DIR="$TMP/cache"
export AETHER_CACHE_DIR
unset AETHER_HOME
AE_CC="$zig cc"
export AE_CC
fails=0
fail() { echo "  [FAIL] $NAME: $1"; fails=$((fails + 1)); }

# 508 nested `match` arms: 1018 brackets of C, past Clang's default 256.
awk 'BEGIN { print "main() {\n    x = 0"
             for (i = 0; i < 508; i++) print "match x {\n0 -> {"
             print "x = 0"
             for (i = 0; i < 508; i++) print "}\n_ -> {}\n}"
             print "    println(\"matches ${x}\")\n}" }' > "$TMP/deep.ae"

# The compile line must carry -fbracket-depth, which zig's clang takes, and
# the program must build and run. zig 0.16's clang sets a default above 256,
# so with it the C compiles either way, and the flag on the line is what
# tells the probe read the compiler right. (Where the runtime archive is
# another C runtime's, ae compiles the runtime from source for zig, #3813.)
if ! (cd "$TMP" && "$AE" build -v deep.ae -o deep) > "$TMP/log" 2>&1; then
    grep -i "error" "$TMP/log" | head -3 | cut -c1-200 | sed 's/^/        /'
    fail "a program nested 1018 brackets deep did not build with AE_CC=\"zig cc\""
elif [ "$("$TMP/deep" | tr -d '\r')" != "matches 0" ]; then
    fail "the program built with AE_CC=\"zig cc\" did not print \"matches 0\""
fi
grep '^\[cmd\].*-Wformat' "$TMP/log" | tail -1 | grep -q -- '-fbracket-depth=' ||
    fail "the compile line with AE_CC=\"zig cc\" carries no -fbracket-depth"
# #3814: the build cache knows the compiler by `zig cc --version`, zig's
# clang, and not by `zig --version`, which every zig refuses alike.
grep '^\[cache\] ccid: .* --version .*outcome=line line=.*clang version' "$TMP/log" > /dev/null ||
    fail "the cache's compiler identity with AE_CC=\"zig cc\" does not name zig's clang version: $(grep '^\[cache\] ccid:' "$TMP/log" | tail -1 | cut -c1-160)"

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] $NAME: with AE_CC=\"zig cc\" the bracket probe tells, and a program nested 1018 brackets deep builds with -fbracket-depth and runs"
exit 0
