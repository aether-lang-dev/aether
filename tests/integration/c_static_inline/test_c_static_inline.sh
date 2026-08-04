#!/bin/sh
# Regression (#1241): calling a C `static inline` helper from Aether needs no
# hand-written shim. The helper has no linkable symbol, but when the header is
# force-included into the generated translation unit its definition is visible
# there, so the call is inlined.
#
# The prototype Aether emits for the extern (`int fast_double(int);`) follows the
# header's `static inline` definition. C permits that: the later declaration
# inherits the prior internal linkage (C11 6.2.2p4). Verified on both clang and
# gcc frontends, so this is not one compiler being lenient.
#
# This test exists so that stays true. If codegen ever emits something that
# clashes with a static definition, the shim-free path breaks silently and
# downstreams go back to writing one-line C bridges.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] c_static_inline: ae not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

cd "$SCRIPT_DIR" || exit 1
if ! "$AE" build probe.ae -o "$TMPDIR/probe" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] c_static_inline: probe.ae did not build"
    sed 's/^/        /' "$TMPDIR/build.log" | head -10
    exit 1
fi

out=$("$TMPDIR/probe" 2>&1)
if [ "$out" != "ok" ]; then
    echo "  [FAIL] c_static_inline: probe printed '$out', want 'ok'"
    exit 1
fi

echo "  [PASS] c_static_inline: static inline helpers callable with no C shim"
