#!/bin/sh
# #3779: aetherc emits a whole program as one C translation unit, and GCC caps
# how far inlining may grow one unit (inline-unit-growth, 40 percent by
# default). Past the cap GCC refused every later inline in a large program,
# hot or not, and aephysics' joint grid stepped 38% slower. `ae` gives GCC a
# budget sized for a whole program, on the optimising builds:
#
#   - an -O2 build (and --emit=lib) with GCC carries
#     `--param inline-unit-growth=<N>` on the compile line;
#   - an -O0 build (--quick) does not, nor a build with Clang, which has no
#     such cap;
#   - the user's own cflags come after it, so their own
#     `--param inline-unit-growth=` is the one GCC uses.
#
# What each build ran is read from its `-v` log, on the `[cmd]` line that
# compiles the generated C (the only one carrying -Wformat).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=gcc_inline_budget

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] $NAME: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
AETHER_CACHE_DIR="$TMP/cache"
export AETHER_CACHE_DIR
unset AETHER_HOME
fails=0
fail() { echo "  [FAIL] $NAME: $1"; fails=$((fails + 1)); }

mkdir -p "$TMP/proj" "$TMP/withparam" "$TMP/logs"
printf 'main() {\n    println("budget")\n}\n' > "$TMP/proj/p.ae"
printf 'main() {\n    println("budget")\n}\n' > "$TMP/withparam/p.ae"
printf '[build]\ncflags = "--param inline-unit-growth=7"\n' > "$TMP/withparam/aether.toml"

# build <dir> <log> <ae build args...>: a verbose build in <dir>; the compile
# line of the generated C goes to <log>.line. Says so and returns 1 when the
# build fails or no such line is found.
build() {
    dir="$1"; log="$2"; shift 2
    if ! (cd "$dir" && "$AE" build -v "$@") > "$TMP/logs/$log" 2>&1; then
        tail -5 "$TMP/logs/$log" | sed 's/^/        /'
        fail "$log: the build failed"
        return 1
    fi
    grep '^\[cmd\].*-Wformat' "$TMP/logs/$log" | tail -1 > "$TMP/logs/$log.line"
    if [ ! -s "$TMP/logs/$log.line" ]; then
        fail "$log: no compile line of the generated C in the -v log"
        return 1
    fi
    return 0
}
# budgets <log>: the inline-unit-growth values on its compile line, in order.
budgets() { tr ' ' '\n' < "$TMP/logs/$1.line" | sed -n 's/^inline-unit-growth=//p' | tr '\n' ' '; }

# is_gcc <log>: whether the compiler of that line is GCC (not Clang, which
# defines __GNUC__ too).
is_gcc() {
    line="$(sed 's/^\[cmd\] //' "$TMP/logs/$1.line")"
    case "$line" in
        \"*) cc="${line#\"}"; cc="${cc%%\"*}" ;;   # a quoted path, spaces and all
        *) cc="${line%% *}" ;;
    esac
    macros="$(printf '' | "$cc" -dM -E -x c - 2>/dev/null)"
    printf '%s\n' "$macros" | grep -q '__GNUC__' || return 1
    printf '%s\n' "$macros" | grep -q '__clang__' && return 1
    return 0
}

if build "$TMP/proj" o2 p.ae -o p; then
    if is_gcc o2; then
        b="$(budgets o2)"
        [ -n "$b" ] || fail "an -O2 build with GCC has no --param inline-unit-growth on its compile line"
        if build "$TMP/proj" lib p.ae --emit=lib -o plib; then
            [ -n "$(budgets lib)" ] || fail "an --emit=lib build with GCC has no --param inline-unit-growth"
        fi
        # The user's budget is the last on the line, after ae's own.
        if build "$TMP/withparam" user p.ae -o p; then
            set -- $(budgets user)
            if [ "$#" -lt 2 ]; then
                fail "with cflags naming a budget, the line has '$*', not ae's and then the user's"
            else
                eval "last=\${$#}"
                [ "$last" = 7 ] || fail "the user's --param inline-unit-growth=7 is not the last on the line ('$*')"
            fi
        fi
        gcc_seen=1
    else
        [ -z "$(budgets o2)" ] || fail "a build with a C compiler other than GCC got --param inline-unit-growth"
    fi
fi

if build "$TMP/proj" o0 p.ae --quick -o pq; then
    [ -z "$(budgets o0)" ] || fail "an -O0 build (--quick) got --param inline-unit-growth"
fi

# Clang, where one is installed: no budget, and the build still passes.
if command -v clang > /dev/null 2>&1; then
    AE_CC=clang
    export AE_CC
    if build "$TMP/proj" clang p.ae -o pc; then
        [ -z "$(budgets clang)" ] || fail "a build with AE_CC=clang got --param inline-unit-growth"
    fi
    unset AE_CC
    clang_seen=" and Clang"
fi

[ "$fails" -eq 0 ] || exit 1
if [ -n "${gcc_seen:-}" ]; then
    echo "  [PASS] $NAME: GCC's -O2 and --emit=lib builds get the inlining budget, the user's own comes last; -O0${clang_seen:-} get none"
else
    echo "  [PASS] $NAME: the compiler here is not GCC and gets no budget; -O0${clang_seen:-} get none"
fi
exit 0
