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
# entry, which the first build publishes (#2500).
build_expect "first build with gcc A on PATH" 1 PATH="$TMP/ccA:$PATH"
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

# 3. A compiler whose driver file never changes when the compiler behind it
# does (macOS's xcrun shims, a ccache masquerade): the key also folds in the
# first line of `--version`. This wrapper answers --version from a file next
# to it, so its own bytes stay the same while its version changes.
write_version_wrapper() {
    mkdir -p "$1"
    if [ "$WIN" = 1 ]; then
        printf '@if "%%~1"=="--version" (type "%%~dp0version.txt") else ("%s" -DAE_CC_TAG=%s %%*)\r\n' \
            "$(cygpath -w "$REAL_CC")" "$2" > "$1/gcc.cmd"
    else
        printf '#!/bin/sh\nif [ "$1" = "--version" ]; then cat "$(dirname "$0")/version.txt"; exit 0; fi\nexec "%s" -DAE_CC_TAG=%s "$@"\n' \
            "$REAL_CC" "$2" > "$1/gcc"
        chmod +x "$1/gcc"
    fi
}
expect_miss() {
    if grep -q "cache hit" "$TMP/build.log"; then
        echo "  [FAIL] cache_c_compiler_identity: $1"
        sed 's/^/        /' "$TMP/build.log" | head -5
        exit 1
    fi
}
write_version_wrapper "$TMP/ccD" 5
echo "shim gcc 15.1.0" > "$TMP/ccD/version.txt"
CC_D="$(wrapper_file "$TMP/ccD")"
build_expect "first build with a version-reporting \$CC" 5 CC="$CC_D"
build_expect "unchanged rebuild with the version-reporting \$CC" 5 CC="$CC_D"
expect_hit "version shim"
echo "shim gcc 16.0.0" > "$TMP/ccD/version.txt"
build_expect "rebuild after the compiler behind an unchanged shim changed" 5 CC="$CC_D"
expect_miss "a compiler upgrade behind an unchanged driver file was served from the cache"

# 4. Many --extra files: the key text used to be built with unchecked
# snprintf appends into a 2 KiB stack buffer, and about 110 extra files
# overran it. The key holds 17 bytes per file whatever its path, so the
# files are named by short relative paths: the --extra list itself stays
# far below ae's 8 KiB limit on every platform (absolute paths under a
# long temp dir, macOS's, pass it).
mkdir -p m
MANY=""
i=0
while [ $i -lt 130 ]; do
    printf 'int many_%d(void) { return %d; }\n' "$i" "$i" > "m/s$i.c"
    MANY="$MANY --extra m/s$i.c"
    i=$((i + 1))
done
cat > many.ae <<'AEOF'
extern many_129() -> int

main() {
    println("many=${many_129()}")
}
AEOF
if ! "$AE" build many.ae $MANY -o ./many_app >"$TMP/build.log" 2>&1; then
    echo "  [FAIL] cache_c_compiler_identity: a build with 130 --extra files failed"
    sed 's/^/        /' "$TMP/build.log" | head -10
    exit 1
fi
got=$(./many_app 2>&1 | tr -d '\r')
if [ "$got" != "many=129" ]; then
    echo "  [FAIL] cache_c_compiler_identity: 130 --extra files: printed '$got', expected 'many=129'"
    exit 1
fi

echo "  [PASS] cache_c_compiler_identity: a different C compiler rebuilds, the same one hits"
