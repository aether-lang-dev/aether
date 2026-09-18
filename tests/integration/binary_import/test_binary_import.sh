#!/bin/sh
# Integration test: consume a precompiled `--emit=lib` artifact as a
# first-class Aether `import`. Builds libgizmo.so, then both `ae run` and
# `ae build` an app that does `import gizmo` and calls a function export
# plus a builder (trailing-block DSL) export — all resolved by reading
# the artifact's aether_lib_meta catalog and synthesizing an interface
# stub. Asserts the program prints the values returned from the .so.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        echo "  [SKIP] binary_import: Windows DLL hosting is a follow-up"
        exit 0
        ;;
    Darwin) SO_EXT=".dylib" ;;
    *)      SO_EXT=".so" ;;
esac

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK" || true' EXIT
cp "$SCRIPT_DIR/gizmo.ae" "$SCRIPT_DIR/app.ae" "$WORK/"
cd "$WORK"

fail() { echo "  [FAIL] $1"; exit 1; }

# 1. Publish the library as a shared object (native extension per OS).
if ! AETHER_HOME="$ROOT" "$AE" build --emit=lib gizmo.ae -o "libgizmo$SO_EXT" \
        >build_lib.log 2>&1; then
    echo "--- build --emit=lib log:"; cat build_lib.log
    fail "ae build --emit=lib gizmo.ae"
fi
[ -f "libgizmo$SO_EXT" ] || fail "libgizmo$SO_EXT not produced"

# 2. `ae run` the consumer — the binary import is resolved transparently
#    (no gizmo.ae source on the path; only libgizmo.so).
rm -f gizmo.ae   # ensure resolution goes to the .so, not the source
OUT="$(AETHER_HOME="$ROOT" "$AE" run app.ae 2>run.log)" || {
    echo "--- run log:"; cat run.log; fail "ae run app.ae"; }

echo "$OUT" | grep -q "hi world" || { echo "$OUT"; fail "function export gizmo.greet not callable across binary import"; }
echo "$OUT" | grep -q "intro"    || { echo "$OUT"; fail "builder DSL gizmo.section not callable across binary import"; }

# 3. `ae build` to a standalone binary, then run it (rpath must let it
#    find libgizmo.so).
if ! AETHER_HOME="$ROOT" "$AE" build app.ae -o app >build_app.log 2>&1; then
    echo "--- build app log:"; cat build_app.log; fail "ae build app.ae"
fi
OUT2="$(./app 2>&1)" || fail "built binary failed to run"
echo "$OUT2" | grep -q "hi world" || { echo "$OUT2"; fail "built binary: greet missing"; }
echo "$OUT2" | grep -q "intro"    || { echo "$OUT2"; fail "built binary: builder missing"; }

echo "  [PASS] binary_import: function export + builder DSL consumed from a precompiled .so (ae run + ae build)"
