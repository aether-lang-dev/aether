#!/bin/sh
# Effect-tag validation must fail closed when a call graph exceeds the
# analyzer's bounded recursion, and follow every function it reaches.
#
#   deep:      a chain of calls past the depth bound, which is the walk's C
#              stack: refused with the analysis-limit diagnostic.
#   wide:      one function calling 512 others, the last of which reaches
#              `os`. The walk kept the functions it had entered in 512 slots
#              and stopped following past them (#3569), so the tag was
#              refused for a limit; the list now grows, and the tag is
#              refused for the `os` call it reaches.
#   wide_pure: `__pure` of a function calling 600 others, the last of which
#              writes a module global. The purity walk stopped past 512 and
#              said pure (#3569); it is impure.
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] effect_tag_limits: $AE not built"; exit 0; }

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

deep="$TMPDIR/deep.ae"
{
    printf 'import std.os\n\n'
    printf 'deep130() -> os.system("echo hi")\n'
    i=129
    while [ "$i" -ge 0 ]; do
        next=$((i + 1))
        [ "$i" -eq 0 ] && printf '\n@no_os\n'
        printf 'deep%s() -> deep%s()\n' "$i" "$next"
        i=$((i - 1))
    done
    printf '\nmain() { deep0() }\n'
} >"$deep"

wide="$TMPDIR/wide.ae"
{
    printf 'import std.os\n\n'
    i=0
    while [ "$i" -lt 511 ]; do
        printf 'leaf%s() -> 0\n' "$i"
        i=$((i + 1))
    done
    printf 'leaf511() -> os.system("echo hi")\n\n'
    printf '@no_os\nwide() {\n'
    i=0
    while [ "$i" -lt 512 ]; do
        printf '    leaf%s()\n' "$i"
        i=$((i + 1))
    done
    printf '}\n\nmain() { wide() }\n'
} >"$wide"

wide_pure="$TMPDIR/wide_pure.ae"
{
    printf 'extern exit(code: int)\n\nvar counter = 0\n\n'
    i=0
    while [ "$i" -lt 599 ]; do
        printf 'leaf%s() -> int { return %s }\n' "$i" "$i"
        i=$((i + 1))
    done
    printf 'leaf599() -> int {\n    counter = counter + 1\n    return 0\n}\n\n'
    printf 'wide() -> int {\n    n = 0\n'
    i=0
    while [ "$i" -lt 600 ]; do
        printf '    n = n + leaf%s()\n' "$i"
        i=$((i + 1))
    done
    printf '    return n\n}\n\n'
    printf 'main() {\n'
    printf '    if __pure(wide) == true {\n'
    printf '        println("FAIL: a function whose 600th callee writes a global is pure")\n'
    printf '        exit(1)\n    }\n'
    printf '    println("impure")\n}\n'
} >"$wide_pure"

failed=0
check_rejected() {
    name="$1"
    src="$2"
    want="$3"
    log="$TMPDIR/$name.log"
    if "$AE" build "$src" -o "$TMPDIR/$name" >"$log" 2>&1; then
        echo "  [FAIL] effect_tag_limits: $name compiled with an effect it forbids"
        failed=1
        return
    fi
    if ! grep -q "$want" "$log"; then
        echo "  [FAIL] effect_tag_limits: $name failed without \"$want\""
        sed 's/^/    /' "$log" | head -10
        failed=1
    fi
}

check_rejected deep "$deep" "effect analysis exceeded its call-graph limit"
check_rejected wide "$wide" "reaches a \`os\` operation"

out=$("$AE" run "$wide_pure" 2>&1 | tr -d '\r') || true
if [ "$out" != "impure" ]; then
    echo "  [FAIL] effect_tag_limits: wide_pure:"
    printf '%s\n' "$out" | head -10 | sed 's/^/    /'
    failed=1
fi
[ "$failed" -eq 0 ] || exit 1

echo "  [PASS] effect_tag_limits: the depth bound fails closed, and a wide call graph is followed whole"
