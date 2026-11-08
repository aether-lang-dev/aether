#!/bin/sh
# Regression: builder/ctx codegen must cast at the two int↔void* boundaries
# that the universal-void*-handle lowering introduces:
#   1. `void* _bcfg = (void*)(intptr_t)factory();` (int-returning factory)
#   2. `fn((int)(intptr_t)_builder, ...);`           (int-typed callee param)
#
# Pre-fix the generated C had bare int↔pointer conversions and gcc fails
# under GCC 14+/MinGW's default -Werror=int-conversion. We invoke aetherc
# directly to emit the .c, then explicitly compile it with
# -Werror=int-conversion (which `ae build` does not currently surface
# as a env knob).  See builder-ctx-handle-void-ptr-int-conversion.md.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cd "$SCRIPT_DIR" || exit 1

c_file="$tmpdir/probe.c"
emit_log="$tmpdir/emit.log"
if ! "$AETHERC" probe.ae "$c_file" >"$emit_log" 2>&1; then
    echo "  [FAIL] builder_int_factory_void_ptr_cast: aetherc failed to emit C"
    sed 's/^/    /' "$emit_log" | head -20
    exit 1
fi

inc="-I$ROOT/runtime -I$ROOT/runtime/actors -I$ROOT/std -I$ROOT/std/collections"
gcc_log="$tmpdir/gcc.log"
# Compile the generated C with -Werror=int-conversion to catch any unchecked
# int↔ptr conversions emitted by the builder/ctx lowering. The sink isn't
# under test here, so don't bother linking — the regression is purely about
# the generated C being clean under the harder warning.
if ! ${CC:-cc} -c -Werror=int-conversion $inc "$c_file" -o "$tmpdir/probe.o" >"$gcc_log" 2>&1; then
    echo "  [FAIL] builder_int_factory_void_ptr_cast: gcc -Werror=int-conversion rejected the generated C"
    sed 's/^/    /' "$gcc_log" | head -40
    exit 1
fi

echo "  [PASS] builder_int_factory_void_ptr_cast: int-factory + int-callee compile clean under -Werror=int-conversion"
exit 0
