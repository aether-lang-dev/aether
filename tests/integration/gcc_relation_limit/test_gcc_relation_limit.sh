#!/bin/sh
# Regression (#3848): every GCC compile `ae` makes carries
# `--param relation-block-limit=0`, and `ae cflags` prints it.
#
# GCC 12 and later record the false edge of a float comparison `a > n` as the
# relation `a <= n`, which a NaN breaks, and fold later comparisons from the
# chain: at -O1 and up a program's float comparisons answered wrong
# (float_order_nan checks the answers). The limit turns the relations off.
#
#   1. An -O2 build carries it, also when the user names an inlining budget.
#   2. An -O0 build (--quick) carries it too: the user's own cflags can say
#      -O2 there, and a --quick build with `cflags = "-O2"` is checked.
#   3. `ae cflags` prints it, for C built by hand from generated C.
#   4. nan_chain.c, the reduction and a variant whose comparisons meet only
#      along a jump-threaded path, compiled at -O3 with the flags `ae cflags`
#      gives, answers 3 from each.
#   5. `ae cflags --cc <command>` asks that compiler, not the one `ae`
#      drives: the GCC prints the limit, the same GCC made to look like
#      Clang (-D__clang__) does not.
#   6. A cross build whose compiler is a GCC (a stub `zig` here, whose
#      `zig cc` is this GCC) carries it on its compiles too.
# A compiler that is not GCC 12+ gets none, and the check skips.

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
mkdir -p "$TMP/plain" "$TMP/named" "$TMP/o2flags"
printf 'main() {\n    println("relation")\n}\n' > "$TMP/plain/p.ae"
cp "$TMP/plain/p.ae" "$TMP/named/p.ae"
cp "$TMP/plain/p.ae" "$TMP/o2flags/p.ae"
printf '[build]\ncflags = "--param inline-unit-growth=7"\n' > "$TMP/named/aether.toml"
printf '[build]\ncflags = "-O2"\n' > "$TMP/o2flags/aether.toml"

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
    has_limit o0 || { echo "  [FAIL] $NAME: an -O0 build (--quick) has no relation limit"; fail=1; }
else
    echo "  [FAIL] $NAME: the -O0 build failed"; fail=1
fi
if build "$TMP/o2flags" o2flags p.ae --quick -o pq; then
    has_limit o2flags ||
        { echo "  [FAIL] $NAME: a --quick build whose cflags say -O2 has no relation limit"; fail=1; }
else
    echo "  [FAIL] $NAME: the --quick build with cflags -O2 failed"; fail=1
fi

# `ae cflags`, and the reduction built by hand with what it prints.
flags="$(cd "$TMP" && AETHER_CACHE_DIR="$TMP/cache_cflags" AETHER_HOME= "$AE" cflags --cflags 2>/dev/null)"
case " $flags " in
    *" --param relation-block-limit=0 "*) ;;
    *) echo "  [FAIL] $NAME: ae cflags does not print --param relation-block-limit=0"; fail=1 ;;
esac
# shellcheck disable=SC2086 # the flags are words, as in `gcc your.c $(ae cflags)`
if $CC_USED -O3 $flags "$SCRIPT_DIR/nan_chain.c" -o "$TMP/nan_chain" > "$TMP/nan_chain.log" 2>&1; then
    got="$("$TMP/nan_chain" | tr -d '\r')"
    [ "$got" = "3 3" ] ||
        { echo "  [FAIL] $NAME: nan_chain.c at -O3 with ae cflags answered '$got', not '3 3'"; fail=1; }
else
    echo "  [FAIL] $NAME: nan_chain.c did not compile with ae cflags"
    tail -5 "$TMP/nan_chain.log" | sed 's/^/        /'
    fail=1
fi

# `ae cflags --cc`: the flags of the compiler named, asked of it.
# cflags_for <tag> <command>: what `ae cflags --cflags --cc <command>` prints.
cflags_for() {
    (cd "$TMP" && AETHER_CACHE_DIR="$TMP/cache_cc_$1" AETHER_HOME= "$AE" cflags --cflags --cc "$2" 2>&1)
}
got="$(cflags_for gcc "$CC_USED")"
case " $got " in
    *" --param relation-block-limit=0 "*) ;;
    *) echo "  [FAIL] $NAME: ae cflags --cc $CC_USED does not print the limit: $got"; fail=1 ;;
esac
got="$(cflags_for clang "$CC_USED -D__clang__=1")"
case " $got " in
    *"relation-block-limit"*)
        echo "  [FAIL] $NAME: ae cflags --cc naming a compiler that is not GCC prints the limit"; fail=1 ;;
    *" -I"*) ;;
    *) echo "  [FAIL] $NAME: ae cflags --cc '$CC_USED -D__clang__=1' failed: $got"; fail=1 ;;
esac

# A cross build with a GCC for its compiler. The stub `zig` answers
# `version`, runs this GCC (without `-target`) for ae's probes, and logs
# every other compile and link, writing its output empty.
STUB="$TMP/stubzig"
mkdir -p "$STUB"
cat > "$STUB/zig" <<EOF
#!/bin/sh
case "\$1" in
    version) echo 0.16.0; exit 0 ;;
    cc) shift ;;
    ar) shift; for a in "\$@"; do case "\$a" in *.a) : > "\$a" ;; esac; done; exit 0 ;;
    *) exit 0 ;;
esac
skip=0
for a in "\$@"; do
    shift
    if [ "\$skip" = 1 ]; then skip=0; continue; fi
    if [ "\$a" = -target ]; then skip=1; continue; fi
    set -- "\$@" "\$a"
done
case " \$* " in
    *" -fsyntax-only "*|*_probe.c*|*" --version "*|*" -dumpversion "*) exec $CC_USED "\$@" ;;
esac
echo "ZIGCC: \$*" >> "$TMP/zigcc.log"
out=""; prev=""
for a in "\$@"; do [ "\$prev" = -o ] && out="\$a"; prev="\$a"; done
[ -n "\$out" ] && : > "\$out"
exit 0
EOF
chmod +x "$STUB/zig"
# Windows spawns a program by its extension: a .cmd that hands over to sh.
printf '@sh "%%~dp0zig" %%*\r\n' > "$STUB/zig.cmd"
: > "$TMP/zigcc.log"
(cd "$TMP/plain" && PATH="$STUB:$PATH" AETHER_CACHE_DIR="$TMP/cache_cross" AETHER_HOME= \
    "$AE" build --target=x86_64-linux p.ae -o px) > "$TMP/cross.log" 2>&1 || :
if ! grep -q '^ZIGCC: ' "$TMP/zigcc.log"; then
    echo "  [FAIL] $NAME: the cross build ran no compile through the stub zig"
    tail -5 "$TMP/cross.log" | sed 's/^/        /'
    fail=1
elif grep '^ZIGCC: ' "$TMP/zigcc.log" | grep -- ' -c ' | grep -v -q -- "--param relation-block-limit=0"; then
    echo "  [FAIL] $NAME: a cross compile with a GCC has no --param relation-block-limit=0"
    grep '^ZIGCC: ' "$TMP/zigcc.log" | grep -- ' -c ' | grep -v -- "relation-block-limit" | head -2 |
        cut -c1-200 | sed 's/^/        /'
    fail=1
fi
[ "$fail" -eq 0 ] || exit 1
echo "  [PASS] $NAME: every GCC compile, cross included, and ae cflags (--cc too) carry --param relation-block-limit=0; -O3 answers right with it"
exit 0
