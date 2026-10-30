#!/bin/sh
# Effect-tag validation must fail closed when a call graph exceeds the
# analyzer's bounded recursion or visited-function storage.
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] effect_tag_limits: $AE not built"; exit 0; }

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

failed=0
check_rejected() {
    name="$1"
    src="$2"
    log="$TMPDIR/$name.log"
    if "$AE" build "$src" -o "$TMPDIR/$name" >"$log" 2>&1; then
        echo "  [FAIL] effect_tag_limits: $name compiled after analysis stopped"
        failed=1
        return
    fi
    if ! grep -q "effect analysis exceeded its call-graph limit" "$log"; then
        echo "  [FAIL] effect_tag_limits: $name failed without the analysis-limit diagnostic"
        sed 's/^/    /' "$log" | head -10
        failed=1
    fi
}

check_rejected deep "$deep"
check_rejected wide "$wide"
[ "$failed" -eq 0 ] || exit 1

echo "  [PASS] effect_tag_limits: depth and visited-function limits fail closed"
