#!/bin/sh
# Regression (#2539): names and paths of any length reach the generated C,
# the compiler and the cache whole, or are refused with a reason.
#
# 1. Two functions with 310-byte names that share their first 260 bytes,
#    and a struct with a 300-byte name. Codegen built names in 256-byte
#    buffers: both calls named one cut identifier that nothing defined, and
#    the struct's C type was cut, so the C compile failed.
# 2. A --lib directory 300 bytes long holding the imported module. ae and
#    the compiler each copied it into 256 bytes, searched a directory
#    nobody named, and reported the module missing. Windows opens no path
#    that long, so there only ae's hand-off to the compiler is checked.
# 3. An AETHER_CACHE_DIR 600 bytes long: it was cut to 511 bytes, another
#    directory. It is refused now, saying so.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] long_names_and_paths: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
AETHER_CACHE_DIR="$TMP/cache"
export AETHER_CACHE_DIR
fail=0

rep() {
    r=""
    i=0
    while [ $i -lt "$2" ]; do r="$r$1"; i=$((i + 1)); done
    printf '%s' "$r"
}

# --- 1. long function and struct names ---
mkdir -p "$TMP/p1"
cd "$TMP/p1" || exit 1
prefix=$(rep a 260)
one="${prefix}$(rep b 50)"
two="${prefix}$(rep c 50)"
sname="S$(rep q 299)"
{
    printf '%s() -> int { return 1 }\n' "$one"
    printf '%s() -> int { return 2 }\n\n' "$two"
    printf 'struct %s {\n    v: int\n}\n\n' "$sname"
    printf 'main() {\n'
    printf '    p = %s { v: 3 }\n' "$sname"
    printf '    println("${%s()} ${%s()} ${p.v}")\n' "$one" "$two"
    printf '}\n'
} > main.ae
got=$("$AE" run main.ae 2>"$TMP/run1.err" | tr -d '\r')
if [ "$got" != "1 2 3" ]; then
    echo "  [FAIL] long_names_and_paths: 310-byte functions and a 300-byte struct printed '$got', expected '1 2 3'"
    sed 's/^/        /' "$TMP/run1.err" | cut -c1-200 | head -5
    fail=1
else
    echo "  [PASS] long_names_and_paths: names past 256 bytes reach the generated C whole"
fi

# --- 2. a --lib directory past 256 bytes ---
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*)
        # Windows opens no path that long, so only ae's hand-off to the
        # compiler is checked here: the whole directory on its command.
        mkdir -p "$TMP/p2"
        cd "$TMP/p2" || exit 1
        printf 'main() {\n    println("two")\n}\n' > main.ae
        libdir="lib_$(rep z 296)"
        "$AE" run --verbose --lib "$libdir" main.ae >"$TMP/run2.out" 2>&1
        if ! grep -q -- "--lib \"$libdir\"" "$TMP/run2.out"; then
            echo "  [FAIL] long_names_and_paths: a 300-byte --lib directory did not reach the compiler whole"
            fail=1
        else
            echo "  [PASS] long_names_and_paths: a 300-byte --lib directory reaches the compiler whole"
        fi
        ;;
    *)
        libdir="$TMP/p2/$(rep l 200)/$(rep m 100)"
        mkdir -p "$libdir/greet" "$TMP/p2/src"
        printf 'exports (msg)\n\nmsg() -> string {\n    return "found"\n}\n' > "$libdir/greet/module.ae"
        printf 'import greet\n\nmain() {\n    println(greet.msg())\n}\n' > "$TMP/p2/src/main.ae"
        cd "$TMP/p2" || exit 1
        got=$("$AE" run --lib "$libdir" src/main.ae 2>"$TMP/run2.err" | tr -d '\r')
        if [ "$got" != "found" ]; then
            echo "  [FAIL] long_names_and_paths: a module in a ${#libdir}-byte --lib directory was not found ('$got')"
            sed 's/^/        /' "$TMP/run2.err" | cut -c1-200 | head -5
            fail=1
        else
            echo "  [PASS] long_names_and_paths: a module in a ${#libdir}-byte --lib directory is found"
        fi
        ;;
esac

# --- 3. an AETHER_CACHE_DIR past 512 bytes ---
mkdir -p "$TMP/p3"
cd "$TMP/p3" || exit 1
printf 'main() {\n    println("three")\n}\n' > main.ae
long_cache="$TMP/$(rep k 600)"
if AETHER_CACHE_DIR="$long_cache" "$AE" run main.ae >"$TMP/run3.out" 2>"$TMP/run3.err"; then
    echo "  [FAIL] long_names_and_paths: a 600-byte AETHER_CACHE_DIR was used as if it fit"
    fail=1
elif ! grep -q "Set AETHER_CACHE_DIR to a shorter path" "$TMP/run3.err"; then
    echo "  [FAIL] long_names_and_paths: a 600-byte AETHER_CACHE_DIR failed without saying why"
    sed 's/^/        /' "$TMP/run3.err" | cut -c1-200 | head -5
    fail=1
else
    echo "  [PASS] long_names_and_paths: a 600-byte AETHER_CACHE_DIR is refused, saying why"
fi

exit $fail
