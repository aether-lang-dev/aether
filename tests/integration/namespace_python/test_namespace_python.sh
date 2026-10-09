#!/bin/sh
# End-to-end Python SDK generator test. `ae build --namespace` emits
# both the .so and the .py module; the Python script imports the
# module and exercises the full surface.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

# find_python.sh, not `command -v python3`: Windows has no python3.exe from
# the official installer, and a Store alias of that name is not Python.
PY="$(sh "$ROOT/tests/scripts/find_python.sh" 2>/dev/null)" || PY=""
if [ -z "$PY" ]; then
    echo "  [SKIP] test_namespace_python (no working Python 3)"
    exit 0
fi

case "$(uname -s 2>/dev/null)" in
    Darwin) LIB_EXT=".dylib" ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT) LIB_EXT=".dll" ;;
    *)      LIB_EXT=".so" ;;
esac

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT
pass=0; fail=0

cp "$SCRIPT_DIR"/manifest.ae "$SCRIPT_DIR"/calc.ae "$SCRIPT_DIR"/check.py "$TMPDIR/"

cd "$TMPDIR"
if ! AETHER_HOME="" "$ROOT/build/ae" build --namespace . >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] ae build --namespace ."; cat "$TMPDIR/build.log"; fail=$((fail + 1))
elif [ ! -f "$TMPDIR/calc_generated_sdk.py" ]; then
    echo "  [FAIL] calc_generated_sdk.py was not generated"; ls -la "$TMPDIR"; fail=$((fail + 1))
else
    LIB_PATH=""
    for c in "$TMPDIR/lib"*"${LIB_EXT}"; do
        [ -f "$c" ] && { LIB_PATH="$c"; break; }
    done
    if [ -z "$LIB_PATH" ]; then
        echo "  [FAIL] no namespace lib"; fail=$((fail + 1))
    elif $PY "$TMPDIR/check.py" "$LIB_PATH" >"$TMPDIR/run.out" 2>&1; then
        echo "  [PASS] generated Python SDK round-trips"; pass=$((pass + 1))
        # Confirm Aether-side console output reaches the host's stdout.
        # Aether script prints `[ae] ...` lines from its function bodies;
        # the host's check.py prints untagged status. Both should appear.
        if grep -qF "[ae] double_it" "$TMPDIR/run.out"; then
            echo "  [PASS] Aether [ae] script-side output visible to host"
            pass=$((pass + 1))
        else
            echo "  [FAIL] Aether [ae] script-side output missing from stdout"
            cat "$TMPDIR/run.out"
            fail=$((fail + 1))
        fi
    else
        echo "  [FAIL] python check.py failed"; cat "$TMPDIR/run.out"; fail=$((fail + 1))
    fi
fi

echo ""
echo "namespace_python: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
