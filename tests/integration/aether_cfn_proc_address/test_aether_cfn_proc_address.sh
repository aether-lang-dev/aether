#!/bin/sh
# #2200: `cfn Name(...) -> R`, the loader shape. A module declares a named C
# function-pointer type, keeps the loaded pointer in a module-level `var`,
# fills it from a C `get_proc_address(name)` (the wglGetProcAddress /
# vkGetDeviceProcAddr / dlsym shape) and calls through it; the program that
# imports the module also takes a `cfn`-typed value back across the import
# boundary and calls it. Compiled, linked against a C glue file that defines
# the entry points, run, and its output compared.
#
# Before this, a call through a module-level function-pointer `var` was
# emitted bare on a `void*` (C rejected it), and the same call inside an
# IMPORTED module failed the checker with "Undefined function": the cell was
# merged as `<module>_name` but the call kept the bare name.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] aether_cfn_proc_address: toolchain not built"
    exit 0
fi

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] aether_cfn_proc_address on Windows: -ldl unavailable"
        exit 0
        ;;
esac

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir" || true' EXIT

cat > "$tmpdir/glx.ae" <<'AE'
module glx

exports (GenBuffers, BufferData, load, gen, lookup)

extern get_proc_address(name: string) -> ptr

cfn GenBuffers(n: int, ids: ptr)
cfn BufferData(target: int, size: long, data: ptr, usage: int)

var gen_buffers: GenBuffers = null
var buffer_data: BufferData = null

load() {
    gen_buffers = get_proc_address("glGenBuffers") as GenBuffers
    buffer_data = get_proc_address("glBufferData") as BufferData
}

gen(ids: ptr) {
    gen_buffers(1, ids)
    buffer_data(34962, 16, null, 35044)
}

lookup(name: string) -> GenBuffers {
    return get_proc_address(name) as GenBuffers
}
AE

cat > "$tmpdir/app.ae" <<'AE'
import glx
extern calloc(n: long, size: long) -> ptr
extern free(p: ptr)

main() {
    glx.load()
    ids = calloc(1, 4)
    glx.gen(ids)
    println("buffer=${(ids as int[])[0]}")
    let g: GenBuffers = glx.lookup("glGenBuffers")
    g(2, ids)
    println("buffer2=${(ids as int[])[0]}")
    free(ids)
}
AE

cat > "$tmpdir/glue.c" <<'C'
#include <stdio.h>
#include <string.h>
static void gen_buffers(int n, void* ids) { ((int*)ids)[0] = 40 + n; }
static void buffer_data(int target, long size, void* data, int usage) {
    printf("buffer_data target=%d size=%ld data=%s usage=%d\n",
           target, size, data ? "set" : "null", usage);
}
void* get_proc_address(const char* name) {
    if (strcmp(name, "glGenBuffers") == 0) return (void*)gen_buffers;
    if (strcmp(name, "glBufferData") == 0) return (void*)buffer_data;
    return 0;
}
C

if ! (cd "$tmpdir" && "$AETHERC" app.ae app.gen.c 2>"$tmpdir/app.err"); then
    echo "  [FAIL] aether_cfn_proc_address: aetherc returned non-zero"
    sed 's/^/    /' "$tmpdir/app.err"
    exit 1
fi

# The module's global is the merged `glx_gen_buffers` cell, and every call
# through it carries the typed C cast.
if ! grep -qE '\(\(void *\(\*\) *\(int, *void\*\)\) *\(glx_gen_buffers\)\) *\(1, *ids\)' "$tmpdir/app.gen.c"; then
    echo "  [FAIL] aether_cfn_proc_address: expected a typed cast on the call through the module var, got:"
    grep -E 'gen_buffers' "$tmpdir/app.gen.c" | sed 's/^/    /'
    exit 1
fi

INCLUDES=""
for d in "$ROOT" "$ROOT/runtime" "$ROOT/runtime/actors" "$ROOT/runtime/scheduler" \
         "$ROOT/runtime/memory" "$ROOT/runtime/utils" "$ROOT/runtime/config" \
         "$ROOT/runtime/simd" "$ROOT/std" "$ROOT/std/string" "$ROOT/std/io" \
         "$ROOT/std/math" "$ROOT/std/collections" "$ROOT/std/mem" \
         "$ROOT/std/bytes" "$ROOT/std/log"; do
    INCLUDES="$INCLUDES -I$d"
done

if ! ${CC:-gcc} $INCLUDES -o "$tmpdir/app_bin" "$tmpdir/app.gen.c" "$tmpdir/glue.c" \
        "$ROOT/build/libaether.a" -lpthread -ldl -lm 2>"$tmpdir/cc.err"; then
    echo "  [FAIL] aether_cfn_proc_address: C compile returned non-zero"
    sed 's/^/    /' "$tmpdir/cc.err"
    exit 1
fi

if ! "$tmpdir/app_bin" > "$tmpdir/app.out" 2>"$tmpdir/app.runerr"; then
    echo "  [FAIL] aether_cfn_proc_address: binary returned non-zero"
    sed 's/^/    /' "$tmpdir/app.runerr"
    exit 1
fi

expected="buffer_data target=34962 size=16 data=null usage=35044
buffer=41
buffer2=42"
got="$(cat "$tmpdir/app.out")"
if [ "$got" != "$expected" ]; then
    echo "  [FAIL] aether_cfn_proc_address: output mismatch"
    echo "  expected:" ; echo "$expected" | sed 's/^/    /'
    echo "  got:"      ; echo "$got"      | sed 's/^/    /'
    exit 1
fi

echo "  [PASS] aether_cfn_proc_address"
exit 0
