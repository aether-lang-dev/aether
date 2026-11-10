#!/bin/sh
# #2691: a closure capturing an array a `try` body writes compiles clean.
#
# Such an array is declared with volatile elements so its stores survive
# the longjmp of a panic (#2618). The closure copied it with memcpy, whose
# plain source pointer discards the qualifier, and on the non-GCC path
# (AETHER_GCC_COMPAT 0: MSVC, emscripten) the constructor
# `_aether_make_closure_N` took it as a plain `const char**` / `int*`,
# which C compilers warn about (GCC -Wdiscarded-qualifiers, clang
# -Wincompatible-pointer-types-discards-qualifiers, MSVC C4090). Copying
# such an array into another one (`b = a`, `int[2] c = a`) and zeroing the
# tail of a shorter literal stored into it went through memmove, memcpy and
# memset the same way.
#
# The generated C is compiled both ways, the GCC path as the C compiler
# picks it and the non-GCC one forced, and any qualifier warning fails the
# test; the program is then built and run.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
[ -f "$AETHERC.exe" ] && AETHERC="$AETHERC.exe"
AE="$ROOT/build/ae"
[ -f "$AE.exe" ] && AE="$AE.exe"
CC_BIN="${CC:-cc}"
command -v "$CC_BIN" >/dev/null 2>&1 || CC_BIN=gcc

if [ ! -f "$AETHERC" ] || [ ! -f "$AE" ]; then
    echo "  [SKIP] closure_volatile_array_capture: toolchain not built"
    exit 0
fi
if ! command -v "$CC_BIN" >/dev/null 2>&1; then
    echo "  [SKIP] closure_volatile_array_capture: no C compiler"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
import std.string

capture(w: string) -> fn {
    names = ["lit", "lit"]
    nums = [1, 2]
    other = [3, 4]
    try {
        names[0] = string.concat(w, "a")
        nums = other
        nums = [7]
        panic("stop")
    } catch e {
        println(e)
    }
    int[2] copy = nums
    f = || {
        return string.length(names[0]) + nums[0] + nums[1] + copy[0]
    }
    return f
}

main() {
    g = capture("w")
    int n = call(g)
    println("${n}")
}
AE

fail() {
    echo "  [FAIL] closure_volatile_array_capture: $1"
    exit 1
}

if ! "$AETHERC" "$tmp/main.ae" "$tmp/main.c" >"$tmp/aetherc.log" 2>&1; then
    sed 's/^/    /' "$tmp/aetherc.log" | head -20
    fail "aetherc failed"
fi
{ echo "#define AETHER_GCC_COMPAT 0"; cat "$tmp/main.c"; } > "$tmp/main_nogcc.c"

INC="-I$ROOT -I$ROOT/runtime -I$ROOT/runtime/utils -I$ROOT/runtime/actors -I$ROOT/runtime/config -I$ROOT/runtime/scheduler -I$ROOT/runtime/memory -I$ROOT/std -I$ROOT/std/collections -I$ROOT/std/string"

for src in main.c main_nogcc.c; do
    # shellcheck disable=SC2086
    if ! $CC_BIN -fsyntax-only $INC "$tmp/$src" >"$tmp/cc.log" 2>&1; then
        sed 's/^/    /' "$tmp/cc.log" | head -20
        fail "$src does not compile"
    fi
    if grep -qi -E "discards|volatile" "$tmp/cc.log"; then
        grep -i -E "discards|volatile" "$tmp/cc.log" | sed 's/^/    /' | head -10
        fail "$src: a volatile array is passed where a plain one is expected"
    fi
done

if ! "$AE" build "$tmp/main.ae" -o "$tmp/main" >"$tmp/build.log" 2>&1; then
    sed 's/^/    /' "$tmp/build.log" | head -20
    fail "ae build failed"
fi
out="$("$tmp/main" 2>&1 | tr -d '\r')"
want="stop
16"
[ "$out" = "$want" ] || fail "got '$out', want '$want'"

echo "  [PASS] closure_volatile_array_capture: a try-written array is captured and copied cleanly on both closure paths"
