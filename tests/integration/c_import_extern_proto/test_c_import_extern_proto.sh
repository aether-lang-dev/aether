#!/bin/sh
# Integration test for `extern ... @c_import` (#1239, #1241).
#
# Asserts that @c_import on an extern function:
#   1. emits NO prototype of its own, so the C header's exact typedef
#      spelling is the only declaration in the translation unit;
#   2. still builds and runs correctly (call sites and casts unaffected);
#   3. makes a `static inline` helper callable with no C shim (#1241).
#
# And, as the control, that dropping the annotation reproduces the failure
# the issues describe: Aether's own ABI-compatible-but-differently-spelled
# prototype (int vs uint8_t, int vs size_t) collides with the header's.
# Without that control the test could pass for the wrong reason, since a
# signature that happens to match exactly compiles either way.
#
# api.h is force-included into every TU (including the generated .gen.c)
# via `cflags = "-include api.h"` in aether.toml.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] c_import_extern_proto: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"; rm -f "$SCRIPT_DIR/noimp_tmp.ae"' EXIT

cd "$SCRIPT_DIR" || exit 1

# (1) + (2) + (3): the annotated probe builds, runs, and prints ok.
if ! "$AE" build probe.ae -o "$TMP/probe" >"$TMP/build.log" 2>&1; then
    echo "  [FAIL] c_import_extern_proto: probe.ae did not build"
    sed 's/^/        /' "$TMP/build.log" | head -15
    exit 1
fi

out=$("$TMP/probe" 2>&1)
if [ "$out" != "ok" ]; then
    echo "  [FAIL] c_import_extern_proto: probe printed '$out', want 'ok'"
    exit 1
fi

# (1) directly: no emitted declaration for any @c_import extern.
"$ROOT/build/aetherc" probe.ae "$TMP/probe.gen.c" >/dev/null 2>&1
if grep -Eq '^[A-Za-z_][A-Za-z0-9_ *]*[ *]api_(scale|span|blob_len|inline_twice)[ ]*\(' "$TMP/probe.gen.c"; then
    echo "  [FAIL] c_import_extern_proto: codegen emitted a prototype for a @c_import extern"
    grep -nE '^[A-Za-z_].*api_(scale|span|blob_len|inline_twice)[ ]*\(' "$TMP/probe.gen.c" | sed 's/^/        /'
    exit 1
fi

# Control: without the annotation the header and the emitted prototype
# conflict. If this ever starts succeeding, the assertion above has stopped
# proving anything and this test needs a sharper probe.
sed 's/ @c_import//' probe.ae > noimp_tmp.ae
if "$AE" build noimp_tmp.ae -o "$TMP/noimp" >"$TMP/noimp.log" 2>&1; then
    echo "  [FAIL] c_import_extern_proto: control built, so @c_import is not what makes probe.ae compile"
    exit 1
fi
if ! grep -q "conflicting types" "$TMP/noimp.log"; then
    echo "  [FAIL] c_import_extern_proto: control failed for an unexpected reason"
    sed 's/^/        /' "$TMP/noimp.log" | head -15
    exit 1
fi

echo "  [PASS] c_import_extern_proto: header owns the prototype, no Aether declaration emitted"
