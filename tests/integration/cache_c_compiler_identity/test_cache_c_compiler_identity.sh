#!/bin/sh
# Regression for #2477: the build cache key must cover the C compiler.
#
# The key folded in the source, aetherc, ae, libaether and the flags, but not
# the C compiler that turns aetherc's output into the binary. Building the
# same source under a different gcc (another one first on PATH, $CC pointing
# elsewhere, an upgrade in place) printed "Built (cache hit)" and handed back
# the binary the previous compiler made, byte for byte.
#
# Each "compiler" here is a wrapper around the real one that adds a -D the
# program prints, so the binary says which compiler built it:
#   1. `gcc` resolved on PATH: put wrapper A first on PATH, then wrapper B.
#   2. $CC naming a wrapper, then that same file rewritten in place.
# Each switch must rebuild; an unchanged build must still hit (a cache that
# never hits would pass the staleness checks while making every build slow).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] cache_c_compiler_identity: ae not built"
    exit 0
fi

REAL_CC="$(command -v gcc 2>/dev/null || command -v cc 2>/dev/null)"
if [ -z "$REAL_CC" ]; then
    echo "  [SKIP] cache_c_compiler_identity: no C compiler on PATH"
    exit 0
fi

WIN=0
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) WIN=1 ;;
esac

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Asserts hits and misses, so the cache must be this test's alone.
AETHER_CACHE_DIR="$TMP/cache"
export AETHER_CACHE_DIR
unset AE_CC CC

# The path a native ae.exe can open; the MSYS spelling elsewhere is the same.
native_path() {
    if [ "$WIN" = 1 ]; then cygpath -m "$1"; else printf '%s' "$1"; fi
}

# write_wrapper DIR TAG: DIR/gcc runs the real compiler with -DAE_CC_TAG=TAG.
# ae.exe spawns natively on Windows, where a shell script is not a program,
# so the wrapper is a .cmd file there.
write_wrapper() {
    mkdir -p "$1"
    if [ "$WIN" = 1 ]; then
        printf '@"%s" -DAE_CC_TAG=%s %%*\r\n' "$(cygpath -w "$REAL_CC")" "$2" > "$1/gcc.cmd"
    else
        printf '#!/bin/sh\nexec "%s" -DAE_CC_TAG=%s "$@"\n' "$REAL_CC" "$2" > "$1/gcc"
        chmod +x "$1/gcc"
    fi
}

wrapper_file() {
    if [ "$WIN" = 1 ]; then native_path "$1/gcc.cmd"; else printf '%s' "$1/gcc"; fi
}

mkdir -p "$TMP/proj"
cd "$TMP/proj" || exit 1

cat > shim.c <<'COF'
int cc_tag(void) {
#ifdef AE_CC_TAG
    return AE_CC_TAG;
#else
    return 0;
#endif
}
COF

cat > main.ae <<'AEOF'
extern cc_tag() -> int

main() {
    n = cc_tag()
    println("tag=${n}")
}
AEOF

# build WHAT EXPECTED [env assignments...]: build, run, compare.
build_expect() {
    what="$1"; want="$2"; shift 2
    if ! env "$@" "$AE" build main.ae --extra shim.c -o ./app >"$TMP/build.log" 2>&1; then
        echo "  [FAIL] cache_c_compiler_identity: build failed ($what)"
        sed 's/^/        /' "$TMP/build.log" | head -10
        exit 1
    fi
    got=$(./app 2>&1 | tr -d '\r')
    if [ "$got" != "tag=$want" ]; then
        echo "  [FAIL] cache_c_compiler_identity: $what"
        echo "         printed '$got', expected 'tag=$want'"
        sed 's/^/        /' "$TMP/build.log" | head -5
        exit 1
    fi
}

write_wrapper "$TMP/ccA" 1
write_wrapper "$TMP/ccB" 2
write_wrapper "$TMP/ccC" 3

expect_hit() {
    if ! grep -q "cache hit" "$TMP/build.log"; then
        echo "  [FAIL] cache_c_compiler_identity: an unchanged rebuild did not hit the cache ($1)"
        sed 's/^/        /' "$TMP/build.log" | head -5
        exit 1
    fi
}

# 1. The compiler found on PATH. The switch is measured against a warm
# entry: the first build of a source writes the depfile that every later key
# is computed from, so it is the second build that publishes the entry the
# third one hits.
build_expect "first build with gcc A on PATH" 1 PATH="$TMP/ccA:$PATH"
build_expect "second build with gcc A" 1 PATH="$TMP/ccA:$PATH"
build_expect "unchanged rebuild with gcc A" 1 PATH="$TMP/ccA:$PATH"
expect_hit "gcc A"
build_expect "stale binary after putting gcc B first on PATH" 2 PATH="$TMP/ccB:$PATH"
build_expect "unchanged rebuild with gcc B" 2 PATH="$TMP/ccB:$PATH"
expect_hit "gcc B"

# 2. $CC naming a compiler, then the same file replaced in place.
CC_C="$(wrapper_file "$TMP/ccC")"
build_expect "first build with \$CC" 3 CC="$CC_C"
build_expect "unchanged rebuild with \$CC" 3 CC="$CC_C"
expect_hit "\$CC"
write_wrapper "$TMP/ccC" 4
build_expect "stale binary after the \$CC compiler changed in place" 4 CC="$CC_C"

echo "  [PASS] cache_c_compiler_identity: a different C compiler rebuilds, the same one hits"
