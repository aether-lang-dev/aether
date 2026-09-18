#!/bin/sh
# Regression: `extern struct Name { ... }` — declare a C struct layout
# in Aether so field access on `ptr as *Name` lowers to native `->field`
# C operations without per-field hand-written accessor functions.
#
# Two flavors covered:
#   - plain fields:     name: type
#   - C bitfields:      name: type : NN
#
# The Aether-side declaration emits a matching C struct typedef into the
# .gen.c, so both Aether and the surrounding C glue see byte-identical
# layouts.  The C side must NOT have a competing typedef under the same
# name in the same translation unit (use a header guard or separate TUs).
#
# Motivation (TODO #2 + #4 from the in-tree mquickjs port): the ~1700
# hand-rolled C accessor functions (`mqjs_jsobj_class_id`,
# `mqjs_jsstring_is_ascii`, etc.) collapse to native Aether `view.field`
# reads once the layout is declared once per struct.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
AE="$ROOT/build/ae"

if [ ! -x "$AETHERC" ] || [ ! -x "$AE" ]; then
    echo "  [SKIP] aether_extern_struct: toolchain not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir" || true' EXIT

# -----------------------------------------------------------------
# Case 1: plain field layout — emit matches a regular C struct typedef.
# -----------------------------------------------------------------
cat > "$tmpdir/case1.ae" <<'AE'
extern struct Point {
    x: int
    y: int
}

extern malloc(n: int) -> ptr
extern free(p: ptr)

main() {
    raw = malloc(8)
    p = raw as *Point
    p.x = 3
    p.y = 4
    println("x=${p.x} y=${p.y}")
    free(raw)
}
AE

if ! "$AETHERC" "$tmpdir/case1.ae" "$tmpdir/case1.gen.c" 2>"$tmpdir/case1.err"; then
    echo "  [FAIL] case 1 (plain fields): aetherc returned non-zero"
    cat "$tmpdir/case1.err" | sed 's/^/    /'
    exit 1
fi

# The emitted C must carry a struct typedef matching the Aether spelling.
if ! grep -qE 'typedef struct Point \{' "$tmpdir/case1.gen.c"; then
    echo "  [FAIL] case 1: expected `typedef struct Point {` in emitted C"
    exit 1
fi
if ! grep -qE 'int x;' "$tmpdir/case1.gen.c" || ! grep -qE 'int y;' "$tmpdir/case1.gen.c"; then
    echo "  [FAIL] case 1: expected plain `int x;` and `int y;` fields"
    grep -E 'struct Point|int x|int y' "$tmpdir/case1.gen.c" | sed 's/^/    /'
    exit 1
fi

# Run it
if ! AETHER_HOME="$ROOT" "$AE" run "$tmpdir/case1.ae" > "$tmpdir/case1.out" 2>&1; then
    echo "  [FAIL] case 1: ae run returned non-zero"
    cat "$tmpdir/case1.out" | sed 's/^/    /'
    exit 1
fi
got=$(cat "$tmpdir/case1.out")
if [ "$got" != "x=3 y=4" ]; then
    echo "  [FAIL] case 1: output mismatch"
    echo "    expected: x=3 y=4"
    echo "    got: $got"
    exit 1
fi

# -----------------------------------------------------------------
# Case 2: signed-int bitfields — emit `field : NN;` and verify read.
# Width-1 signed bitfields read as -1 / 0 (standard C behaviour).
# -----------------------------------------------------------------
cat > "$tmpdir/case2.ae" <<'AE'
extern struct Flags {
    on:  int : 1
    off: int : 1
    val: int : 30
}

extern malloc(n: int) -> ptr

main() {
    raw = malloc(8)
    f = raw as *Flags
    f.on = 1
    f.off = 0
    f.val = 1000
    println("on=${f.on} off=${f.off} val=${f.val}")
}
AE

if ! "$AETHERC" "$tmpdir/case2.ae" "$tmpdir/case2.gen.c" 2>"$tmpdir/case2.err"; then
    echo "  [FAIL] case 2 (signed bitfields): aetherc returned non-zero"
    cat "$tmpdir/case2.err" | sed 's/^/    /'
    exit 1
fi

# Emit must carry C bitfield syntax: `int name : NN;`
if ! grep -qE 'int on : 1;' "$tmpdir/case2.gen.c"; then
    echo "  [FAIL] case 2: expected `int on : 1;` bitfield emit"
    grep -E 'struct Flags|on|off|val' "$tmpdir/case2.gen.c" | sed 's/^/    /'
    exit 1
fi
if ! grep -qE 'int val : 30;' "$tmpdir/case2.gen.c"; then
    echo "  [FAIL] case 2: expected `int val : 30;` bitfield emit"
    exit 1
fi

if ! AETHER_HOME="$ROOT" "$AE" run "$tmpdir/case2.ae" > "$tmpdir/case2.out" 2>&1; then
    echo "  [FAIL] case 2: ae run returned non-zero"
    cat "$tmpdir/case2.out" | sed 's/^/    /'
    exit 1
fi
# Signed 1-bit bitfield: value `1` displays as -1 (high bit = sign).
# On macOS clang this emits a -Wsingle-bit-bitfield-constant-conversion
# warning into the C compile step; the run-output redirect captures it
# via 2>&1. The warning is descriptive ("changes value from 1 to -1") —
# i.e. clang is confirming the same semantics we test for — but its
# presence in the buffer breaks an exact-match. Filter to the program
# output line; the println shape is deterministic.
got=$(grep '^on=' "$tmpdir/case2.out" || true)
if [ "$got" != "on=-1 off=0 val=1000" ]; then
    echo "  [FAIL] case 2: output mismatch (note signed 1-bit semantics)"
    echo "    expected: on=-1 off=0 val=1000"
    echo "    got: $got"
    echo "    full out:"
    cat "$tmpdir/case2.out" | sed 's/^/      /'
    exit 1
fi

# -----------------------------------------------------------------
# Case 3: unsigned bitfields via `byte` — for callers who want the
# natural 0/1 boolean semantics.
# -----------------------------------------------------------------
cat > "$tmpdir/case3.ae" <<'AE'
extern struct UFlags {
    on:  byte : 1
    off: byte : 1
    pad: byte : 6
}

extern malloc(n: int) -> ptr

main() {
    raw = malloc(4)
    f = raw as *UFlags
    f.on = 1
    f.off = 0
    println("on=${f.on} off=${f.off}")
}
AE

if ! "$AETHERC" "$tmpdir/case3.ae" "$tmpdir/case3.gen.c" 2>"$tmpdir/case3.err"; then
    echo "  [FAIL] case 3 (unsigned bitfields): aetherc returned non-zero"
    cat "$tmpdir/case3.err" | sed 's/^/    /'
    exit 1
fi
if ! grep -qE 'unsigned char on : 1;' "$tmpdir/case3.gen.c"; then
    echo "  [FAIL] case 3: expected `unsigned char on : 1;` bitfield emit"
    grep -E 'struct UFlags|on|off' "$tmpdir/case3.gen.c" | sed 's/^/    /'
    exit 1
fi
if ! AETHER_HOME="$ROOT" "$AE" run "$tmpdir/case3.ae" > "$tmpdir/case3.out" 2>&1; then
    echo "  [FAIL] case 3: ae run returned non-zero"
    cat "$tmpdir/case3.out" | sed 's/^/    /'
    exit 1
fi
got=$(cat "$tmpdir/case3.out")
if [ "$got" != "on=1 off=0" ]; then
    echo "  [FAIL] case 3: output mismatch"
    echo "    expected: on=1 off=0"
    echo "    got: $got"
    exit 1
fi

# -----------------------------------------------------------------
# Case 4: end-to-end C-glue interop — Aether declares the layout,
# C glue mutates it via the same struct name, Aether reads back.
# This is the real-world shape (mquickjs's JSObject etc.).
#
# Skipped on Windows MinGW: the final link pulls in libaether's
# `-ldl` dependency which doesn't exist on Windows (no dlopen).
# Cases 1-3 cover the language-level surface; case 4 only adds the
# real-world linking shape, which the rest of the test suite
# already exercises through portable-link paths.
# -----------------------------------------------------------------
SKIP_CASE_4=0
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        SKIP_CASE_4=1
        echo "  [SKIP] case 4 (C-interop link) on Windows: -ldl unavailable"
        ;;
esac

if [ "$SKIP_CASE_4" = 0 ]; then

cat > "$tmpdir/case4.ae" <<'AE'
extern struct Header {
    magic: int
    flags: int : 4
    version: int : 28
}

extern make_header() -> ptr
extern free(p: ptr)

main() {
    p = make_header() as *Header
    println("magic=${p.magic} flags=${p.flags} version=${p.version}")
    free(p)
}
AE

cat > "$tmpdir/case4_glue.c" <<'C'
#include <stdlib.h>

/* The Aether-side `extern struct Header` emits the typedef into
 * the .gen.c.  This C glue file does NOT redeclare Header — it
 * just sees the layout via void* and trusts the field offsets to
 * match what the Aether-side spelling produces.  In a real port,
 * you'd factor the struct into a shared private header. */

struct Header {
    int magic;
    unsigned int flags : 4;
    unsigned int version : 28;
};

void* make_header(void) {
    struct Header* h = malloc(sizeof(struct Header));
    h->magic = 0xCAFE;
    h->flags = 7;     /* 4 bits, max 15 */
    h->version = 42;
    return h;
}
C

if ! "$AETHERC" "$tmpdir/case4.ae" "$tmpdir/case4.gen.c" 2>"$tmpdir/case4.err"; then
    echo "  [FAIL] case 4 (C interop): aetherc returned non-zero"
    cat "$tmpdir/case4.err" | sed 's/^/    /'
    exit 1
fi

# Compile + link with glue.  Need stdlib includes.
INCLUDES=""
for d in "$ROOT" "$ROOT/runtime" "$ROOT/runtime/actors" "$ROOT/runtime/scheduler" \
         "$ROOT/runtime/memory" "$ROOT/runtime/utils" "$ROOT/runtime/config" \
         "$ROOT/runtime/simd" "$ROOT/std" "$ROOT/std/string" "$ROOT/std/io" \
         "$ROOT/std/math" "$ROOT/std/collections" "$ROOT/std/mem" \
         "$ROOT/std/bytes" "$ROOT/std/log"; do
    INCLUDES="$INCLUDES -I$d"
done
# IMPORTANT: case4_glue.c carries its own `struct Header` definition.
# We compile the two TUs separately so neither sees a duplicate typedef:
# - case4.gen.c contains Aether's emitted `typedef struct Header { ... }`
# - case4_glue.c contains the C side's struct
# The Aether-emitted typedef and the C-side struct are byte-identical by
# layout, so the pointer round-trip in main() reads the right offsets.
if ! gcc $INCLUDES -c -o "$tmpdir/case4.gen.o" "$tmpdir/case4.gen.c" \
        2>"$tmpdir/case4.gcc.err"; then
    echo "  [FAIL] case 4: gcc failed compiling .gen.c"
    cat "$tmpdir/case4.gcc.err" | sed 's/^/    /'
    exit 1
fi
if ! gcc -c -o "$tmpdir/case4_glue.o" "$tmpdir/case4_glue.c" \
        2>"$tmpdir/case4.gcc.err"; then
    echo "  [FAIL] case 4: gcc failed compiling glue.c"
    cat "$tmpdir/case4.gcc.err" | sed 's/^/    /'
    exit 1
fi
if ! gcc -o "$tmpdir/case4_bin" "$tmpdir/case4.gen.o" "$tmpdir/case4_glue.o" \
        "$ROOT/build/libaether.a" -lpthread -ldl -lm 2>"$tmpdir/case4.gcc.err"; then
    echo "  [FAIL] case 4: gcc failed linking"
    cat "$tmpdir/case4.gcc.err" | sed 's/^/    /'
    exit 1
fi
if ! "$tmpdir/case4_bin" > "$tmpdir/case4.out" 2>"$tmpdir/case4.runerr"; then
    echo "  [FAIL] case 4: binary returned non-zero"
    cat "$tmpdir/case4.runerr" | sed 's/^/    /'
    exit 1
fi
expected="magic=51966 flags=7 version=42"
got="$(cat "$tmpdir/case4.out")"
if [ "$got" != "$expected" ]; then
    echo "  [FAIL] case 4: output mismatch — Aether and C layouts must agree"
    echo "    expected: $expected"
    echo "    got: $got"
    exit 1
fi
fi  # SKIP_CASE_4 guard

# -----------------------------------------------------------------
# Case 5: regression — non-extern `struct` (the existing Aether-side
# struct definition) still works unchanged.
# -----------------------------------------------------------------
cat > "$tmpdir/case5.ae" <<'AE'
struct Point {
    x: int
    y: int
}

main() {
    p = Point { x: 1, y: 2 }
    println("x=${p.x} y=${p.y}")
}
AE

if ! AETHER_HOME="$ROOT" "$AE" run "$tmpdir/case5.ae" > "$tmpdir/case5.out" 2>&1; then
    echo "  [FAIL] case 5 (regression): ae run failed for native struct"
    cat "$tmpdir/case5.out" | sed 's/^/    /'
    exit 1
fi
got=$(cat "$tmpdir/case5.out")
if [ "$got" != "x=1 y=2" ]; then
    echo "  [FAIL] case 5: native struct regression"
    echo "    expected: x=1 y=2"
    echo "    got: $got"
    exit 1
fi

echo "  [PASS] aether_extern_struct"
exit 0
