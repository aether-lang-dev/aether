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
cp "$SCRIPT_DIR/gizmo.ae" "$SCRIPT_DIR/app.ae" \
   "$SCRIPT_DIR/wrap.ae" "$SCRIPT_DIR/app_transitive.ae" "$WORK/"
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

# 4. TRANSITIVE binary import: the `import gizmo` lives in a NON-ENTRY module
#    (wrap.ae), and the entry (app_transitive.ae) imports only `wrap`. The
#    binary-import prepass must walk the whole import graph, not just the entry
#    file, to discover it — otherwise `import gizmo` is unresolved. This is the
#    adapter/wrapper pattern (a project wraps the engine and imports the wrapper
#    everywhere). gizmo.ae is already removed above, so `gizmo` resolves to the
#    .so; wrap.ae is a source module the prepass must recurse into.
OUT3="$(AETHER_HOME="$ROOT" "$AE" run app_transitive.ae 2>run_t.log)" || {
    echo "--- transitive run log:"; cat run_t.log
    fail "transitive binary import: prepass did not walk into wrap.ae (import gizmo unresolved)"; }
echo "$OUT3" | grep -q "hi world" \
    || { echo "$OUT3"; fail "transitive binary import: wrap.wrapped_greet did not reach gizmo.greet"; }

# And it also links into a standalone binary through the wrapper.
if ! AETHER_HOME="$ROOT" "$AE" build app_transitive.ae -o app_t >build_t.log 2>&1; then
    echo "--- transitive build log:"; cat build_t.log; fail "ae build app_transitive.ae"
fi
echo "$(./app_t 2>&1)" | grep -q "hi world" || fail "built transitive binary: greet missing"

# 5. TRANSITIVE through a DOTTED package import. The wrapper is reached as
#    `import pkg.dotwrap` (a dotted package path), not a flat name. The graph
#    walk must convert `pkg.dotwrap` -> `pkg/dotwrap.ae` and recurse, or the
#    `import gizmo` inside it is never discovered — dotted package imports
#    (what `modules = "."` exists to support) are idiomatic, and a real
#    consumer's chain looks like `import harness.components.phone.validate`.
mkdir -p pkg
printf 'import gizmo\nexports(dgreet)\ndgreet(name: string) -> string { return gizmo.greet(name) }\n' > pkg/dotwrap.ae
printf 'import pkg.dotwrap\nextern println(s: string)\nmain() { println(dotwrap.dgreet("world")) }\n' > app_dotted.ae
OUT4="$(AETHER_HOME="$ROOT" "$AE" run app_dotted.ae 2>run_d.log)" || {
    echo "--- dotted run log:"; cat run_d.log
    fail "dotted transitive binary import: prepass did not follow 'import pkg.dotwrap' (import gizmo unresolved)"; }
echo "$OUT4" | grep -q "hi world" \
    || { echo "$OUT4"; fail "dotted transitive binary import: dotwrap.dgreet did not reach gizmo.greet"; }

echo "  [PASS] binary_import: direct + transitive (flat + DOTTED wrapper) binary import, ae run + ae build"
