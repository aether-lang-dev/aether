#!/bin/sh
# End-to-end Ruby SDK generator test. `ae build --namespace` emits both
# the .so and the Ruby module; the check.rb script imports the module
# and exercises the full surface.
#
# Skips cleanly if ruby isn't installed. Fiddle ships with MRI Ruby
# 1.9.2+ so no extra gem install is needed.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

if ! command -v ruby >/dev/null 2>&1; then
    echo "  [SKIP] test_namespace_ruby (ruby not installed)"
    exit 0
fi

case "$(uname -s 2>/dev/null)" in
    Darwin) LIB_EXT=".dylib" ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT) LIB_EXT=".dll" ;;
    *)      LIB_EXT=".so" ;;
esac

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT
pass=0; fail=0

cp "$SCRIPT_DIR"/manifest.ae "$SCRIPT_DIR"/calc.ae "$SCRIPT_DIR"/check.rb "$TMPDIR/"

cd "$TMPDIR"
if ! AETHER_HOME="" "$ROOT/build/ae" build --namespace . >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] ae build --namespace ."; cat "$TMPDIR/build.log"; fail=$((fail + 1))
elif [ ! -f "$TMPDIR/calc_generated_sdk.rb" ]; then
    echo "  [FAIL] calc_generated_sdk.rb was not generated"; ls -la "$TMPDIR"; fail=$((fail + 1))
else
    LIB_PATH=""
    for c in "$TMPDIR/lib"*"${LIB_EXT}"; do
        [ -f "$c" ] && { LIB_PATH="$c"; break; }
    done
    if [ -z "$LIB_PATH" ]; then
        echo "  [FAIL] no namespace lib"; fail=$((fail + 1))
    elif ruby "$TMPDIR/check.rb" "$LIB_PATH" >"$TMPDIR/run.out" 2>&1; then
        echo "  [PASS] generated Ruby SDK round-trips"; pass=$((pass + 1))
        if grep -qF "[ae] double_it" "$TMPDIR/run.out"; then
            echo "  [PASS] Aether [ae] script-side output visible to host"
            pass=$((pass + 1))
        else
            echo "  [FAIL] Aether [ae] script-side output missing from stdout"
            cat "$TMPDIR/run.out"
            fail=$((fail + 1))
        fi
    else
        echo "  [FAIL] check.rb failed"; cat "$TMPDIR/run.out"; fail=$((fail + 1))
    fi
fi

echo ""
echo "namespace_ruby: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
