#!/bin/sh
# Regression (#3848): every optimising GCC compile of a program carries
# `--param relation-block-limit=0`.
#
# GCC 12 and later record the false edge of a float comparison `a > n` as the
# relation `a <= n`, which a NaN breaks, and fold later comparisons from the
# chain: at -O1 and up a program's float comparisons answered wrong
# (float_order_nan checks the answers). The limit turns the relations off.
# It goes on every optimising build, also when the user names an inlining
# budget of their own, and on no -O0 build. A compiler that is not GCC 12+
# gets none, and the check skips.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
NAME=gcc_relation_limit
AE="$ROOT/build/ae"
[ -x "$AE" ] || AE="$ROOT/build/ae.exe"
if [ ! -x "$AE" ]; then
    echo "  [SKIP] $NAME: ae not built"
    exit 0
fi
CC_USED="${AE_CC:-${CC:-gcc}}"
if $CC_USED -dM -E -x c /dev/null 2>/dev/null | grep -q __clang__ ||
   ! echo 'int x;' | $CC_USED --param relation-block-limit=0 -Werror -x c -fsyntax-only - > /dev/null 2>&1; then
    echo "  [SKIP] $NAME: $CC_USED is not a GCC with --param relation-block-limit"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0
mkdir -p "$TMP/plain" "$TMP/named"
printf 'main() {\n    println("relation")\n}\n' > "$TMP/plain/p.ae"
cp "$TMP/plain/p.ae" "$TMP/named/p.ae"
printf '[build]\ncflags = "--param inline-unit-growth=7"\n' > "$TMP/named/aether.toml"

# build <dir> <tag> <args...>: `ae build -v` into a cache of its own; the
# compile line goes to $TMP/<tag>.log.
build() {
    d="$1"; t="$2"; shift 2
    (cd "$d" && AETHER_CACHE_DIR="$TMP/cache_$t" AETHER_HOME= "$AE" build -v "$@") > "$TMP/$t.log" 2>&1
}
has_limit() { grep '^\[cmd\]' "$TMP/$1.log" | grep -q -- "--param relation-block-limit=0"; }

if build "$TMP/plain" o2 p.ae -o p; then
    has_limit o2 || { echo "  [FAIL] $NAME: an -O2 build has no --param relation-block-limit=0"; fail=1; }
else
    echo "  [FAIL] $NAME: the -O2 build failed"; tail -5 "$TMP/o2.log" | sed 's/^/        /'; fail=1
fi
if build "$TMP/named" named p.ae -o p; then
    has_limit named || { echo "  [FAIL] $NAME: a build naming its own inlining budget lost the relation limit"; fail=1; }
else
    echo "  [FAIL] $NAME: the build with a named budget failed"; fail=1
fi
if build "$TMP/plain" o0 p.ae --quick -o pq; then
    has_limit o0 && { echo "  [FAIL] $NAME: an -O0 build (--quick) got the relation limit"; fail=1; }
else
    echo "  [FAIL] $NAME: the -O0 build failed"; fail=1
fi
[ "$fail" -eq 0 ] || exit 1
echo "  [PASS] $NAME: every optimising GCC build carries --param relation-block-limit=0, -O0 none"
exit 0
