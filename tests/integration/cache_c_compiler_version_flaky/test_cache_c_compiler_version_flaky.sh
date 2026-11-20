#!/bin/sh
# Regression for #3707: the C compiler's `--version` run is a key input, so
# how that run ends must not move the key.
#
# c_compiler_fingerprint folds the first line of `<cc> --version` into the
# build cache key. A run that failed once (a spawn failure, a signal, or an
# exit with nothing on stdout) folded in no line, so that build published
# under another ccid than the next one, and the next unchanged build missed.
# On macOS x86 that run goes wrapper, xcrun shim, clang, and the test of
# #2477 missed now and then on CI.
#
# Each compiler here is a wrapper around the real one:
#   1. `--version` exits 1 with no output once, then answers: the build that
#      saw the failure and the next one must share a key (a hit).
#   2. `--version` never answers (as tcc, which rejects it): still a hit,
#      since the outcome is the same every time.
#   3. POSIX: `--version` killed by a signal once: a hit, as in 1.
#   4. POSIX: `--version` killed by a signal every time: the compiler's
#      identity is not known, so the build is not cached, and still builds.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
SHARED_AE="$ROOT/build/ae"
NAME=cache_c_compiler_version_flaky

if [ ! -x "$SHARED_AE" ] && [ ! -x "$SHARED_AE.exe" ]; then
    echo "  [SKIP] $NAME: ae not built"
    exit 0
fi

REAL_CC="$(command -v gcc 2>/dev/null || command -v cc 2>/dev/null)"
if [ -z "$REAL_CC" ]; then
    echo "  [SKIP] $NAME: no C compiler on PATH"
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

# A toolchain of its own, as cache_c_compiler_identity has: other tests
# rebuild the module archives the key folds in while this one runs.
PRIV="$TMP/tc"
mkdir -p "$PRIV/build"
for e in "$ROOT"/* "$ROOT"/.[!.]*; do
    [ -e "$e" ] || continue
    [ "$(basename "$e")" = build ] && continue
    ln -s "$e" "$PRIV/$(basename "$e")"
done
for f in ae aetherc libaether.a; do
    for g in "$ROOT/build/$f" "$ROOT/build/$f.exe"; do
        [ -f "$g" ] && cp "$g" "$PRIV/build/"
    done
done
for a in "$ROOT"/build/*.a; do
    [ -f "$a" ] || continue
    [ "$(basename "$a")" = libaether_compiler.a ] && continue
    [ -f "$PRIV/build/$(basename "$a")" ] || cp "$a" "$PRIV/build/"
done
if [ -d "$ROOT/build/contrib" ]; then
    mkdir -p "$PRIV/build/contrib"
    for a in "$ROOT"/build/contrib/*.a; do
        [ -f "$a" ] && cp "$a" "$PRIV/build/contrib/"
    done
fi
AE="$PRIV/build/ae"

# write_wrapper DIR MODE: DIR/gcc runs the real compiler, except for
# `--version`, which per MODE:
#   once     exits 1 with nothing on stdout the first time (a file `failed`
#            beside it records that it did), then answers;
#   never    always exits 1 with nothing on stdout;
#   sigonce  is killed by SIGKILL the first time, then answers;
#   sigkill  is always killed by SIGKILL.
# ae.exe spawns natively on Windows, where a shell script is not a program,
# so the wrapper is a .cmd file there (no signals, so only once and never).
write_wrapper() {
    mkdir -p "$1"
    if [ "$WIN" = 1 ]; then
        real="$(cygpath -w "$REAL_CC")"
        case "$2" in
            once)  printf '@if "%%~1"=="--version" if not exist "%%~dp0failed" (type nul > "%%~dp0failed" & exit /b 1)\r\n@"%s" %%*\r\n' "$real" ;;
            never) printf '@if "%%~1"=="--version" exit /b 1\r\n@"%s" %%*\r\n' "$real" ;;
        esac > "$1/gcc.cmd"
        return
    fi
    case "$2" in
        once)    pre='if [ ! -e "$d/failed" ]; then : > "$d/failed"; exit 1; fi' ;;
        never)   pre='exit 1' ;;
        sigonce) pre='if [ ! -e "$d/failed" ]; then : > "$d/failed"; kill -9 $$; fi' ;;
        sigkill) pre='kill -9 $$' ;;
    esac
    printf '#!/bin/sh\nd="$(dirname "$0")"\nif [ "$1" = "--version" ]; then\n    %s\nfi\nexec "%s" "$@"\n' \
        "$pre" "$REAL_CC" > "$1/gcc"
    chmod +x "$1/gcc"
}

wrapper_file() {
    if [ "$WIN" = 1 ]; then cygpath -m "$1/gcc.cmd"; else printf '%s' "$1/gcc"; fi
}

mkdir -p "$TMP/proj"
cd "$TMP/proj" || exit 1
cat > main.ae <<'AEOF'
main() {
    println("built")
}
AEOF

# build WHAT CC: build with $CC, run, and keep the log.
build() {
    [ -f "$TMP/build.log" ] && cp "$TMP/build.log" "$TMP/prev.log"
    if ! CC="$2" "$AE" build -v main.ae -o ./app >"$TMP/build.log" 2>&1; then
        echo "  [FAIL] $NAME: build failed ($1)"
        sed 's/^/        /' "$TMP/build.log" | head -10
        exit 1
    fi
    got=$(./app 2>&1 | tr -d '\r')
    if [ "$got" != "built" ]; then
        echo "  [FAIL] $NAME: $1: the program printed '$got'"
        exit 1
    fi
}

expect_hit() {
    if ! grep -q "cache hit" "$TMP/build.log"; then
        echo "  [FAIL] $NAME: an unchanged rebuild did not hit the cache ($1)"
        echo "        the build before:"
        grep "\[cache\]" "$TMP/prev.log" 2>/dev/null | sed 's/^/          /'
        echo "        this build:"
        grep "\[cache\]" "$TMP/build.log" | sed 's/^/          /'
        exit 1
    fi
}

# 1. --version fails once without output.
write_wrapper "$TMP/once" once
CC_ONCE="$(wrapper_file "$TMP/once")"
build "first build, --version failing once" "$CC_ONCE"
if [ ! -e "$TMP/once/failed" ]; then
    echo "  [FAIL] $NAME: the wrapper's --version never ran, so this test proves nothing"
    exit 1
fi
build "unchanged rebuild, --version answering" "$CC_ONCE"
expect_hit "--version failed once without output"

# 2. --version never answers.
write_wrapper "$TMP/never" never
CC_NEVER="$(wrapper_file "$TMP/never")"
build "first build, --version never answering" "$CC_NEVER"
build "unchanged rebuild, --version never answering" "$CC_NEVER"
expect_hit "a compiler that rejects --version"

if [ "$WIN" = 0 ]; then
    # 3. --version killed once.
    write_wrapper "$TMP/sigonce" sigonce
    CC_SIGONCE="$(wrapper_file "$TMP/sigonce")"
    build "first build, --version killed once" "$CC_SIGONCE"
    build "unchanged rebuild after --version was killed once" "$CC_SIGONCE"
    expect_hit "--version killed by a signal once"

    # 4. --version always killed: not cached, and says so under -v.
    write_wrapper "$TMP/sigkill" sigkill
    CC_SIGKILL="$(wrapper_file "$TMP/sigkill")"
    build "first build, --version always killed" "$CC_SIGKILL"
    build "second build, --version always killed" "$CC_SIGKILL"
    if grep -q "cache hit" "$TMP/build.log"; then
        echo "  [FAIL] $NAME: a build whose compiler identity is unknown was served from the cache"
        exit 1
    fi
    if ! grep -q "did not run to an answer" "$TMP/build.log"; then
        echo "  [FAIL] $NAME: -v did not say why the build is not cached"
        grep "\[cache\]" "$TMP/build.log" | sed 's/^/        /'
        exit 1
    fi
fi

echo "  [PASS] $NAME: a --version run that fails once or never answers keeps the key"
