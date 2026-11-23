#!/bin/sh
# Regression (#3808): an --emit=lib export's result crosses the C ABI as
# its own type, or the export's alias is skipped with a warning; never as
# an int32_t it is not.
#
# A result type the ABI had no spelling for fell back to the int32_t meant
# for an untyped one: a slice result did not compile ("incompatible types
# when initializing type 'int32_t' using type 'AetherSlice'"), an actor
# reference (a pointer) was cut to 32 bits, and a bit set (a 64-bit word)
# lost its high bits. A slice result is now refused as a slice parameter
# is, the library still builds, and a bit set crosses as uint64_t.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
EXE=""
[ -x "$ROOT/build/ae" ] || EXE=".exe"
AE="$ROOT/build/ae$EXE"
AETHERC="$ROOT/build/aetherc$EXE"
if [ ! -x "$AE" ] || [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] emit_lib_result_types: compiler not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0

cat > "$TMP/lib.ae" <<'AE'
enum Bit { B0 = 0, B1 = 1, B40 = 40 }

struct Buf {
    data: ptr
    n: int
}

export view(b: *Buf) -> int[] {
    return (b.data as int[])[0..b.n]
}

export high() -> bit_set[Bit] {
    return bit_set[Bit] { Bit.B0, Bit.B40 }
}

export count(n: int) -> int {
    return n + 1
}

export keep(it: Isolated[int]) -> int {
    return 1
}
AE

# The generated C: no alias for the slice result, a uint64_t one for the
# bit set, the int one as before.
if ! AETHER_HOME="" "$AETHERC" --emit=lib "$TMP/lib.ae" "$TMP/lib.c" > "$TMP/gen.log" 2>&1; then
    echo "  [FAIL] emit_lib_result_types: aetherc --emit=lib failed"
    sed 's/^/        /' "$TMP/gen.log" | head -10
    fail=1
else
    if ! grep -q "function 'view' returns a type that isn't representable in the --emit=lib ABI" "$TMP/gen.log"; then
        echo "  [FAIL] emit_lib_result_types: no warning for the slice result"
        sed 's/^/        /' "$TMP/gen.log" | head -10
        fail=1
    fi
    if grep -q " aether_view(" "$TMP/lib.c"; then
        echo "  [FAIL] emit_lib_result_types: an alias was emitted for the slice result"
        grep -m2 " aether_view(" "$TMP/lib.c" | sed 's/^/        /'
        fail=1
    fi
    if ! grep -q "^uint64_t aether_high(" "$TMP/lib.c"; then
        echo "  [FAIL] emit_lib_result_types: the bit set does not cross as uint64_t"
        grep -m2 "aether_high(" "$TMP/lib.c" | sed 's/^/        /'
        fail=1
    fi
    if ! grep -q "^int32_t aether_count(" "$TMP/lib.c"; then
        echo "  [FAIL] emit_lib_result_types: the int export lost its alias"
        fail=1
    fi
    # The symbol catalog lists only aliases that exist: a skipped one,
    # advertised, sent a host's dlsym to a symbol the library lacks.
    if grep -q '"aether_view"' "$TMP/lib.c"; then
        echo "  [FAIL] emit_lib_result_types: the catalog lists the skipped aether_view"
        grep -m2 '"aether_view"' "$TMP/lib.c" | sed 's/^/        /'
        fail=1
    fi
    if ! grep -q '"aether_count"' "$TMP/lib.c"; then
        echo "  [FAIL] emit_lib_result_types: the catalog does not list aether_count"
        fail=1
    fi
    # Isolated[T] is a move-only compile-time wrapper with no C ABI
    # spelling: its export is skipped, not lowered to the element type.
    if grep -q " aether_keep(" "$TMP/lib.c"; then
        echo "  [FAIL] emit_lib_result_types: an alias was emitted for an Isolated parameter"
        grep -m2 " aether_keep(" "$TMP/lib.c" | sed 's/^/        /'
        fail=1
    fi
fi

# And the library builds.
if ! (cd "$TMP" && AETHER_HOME="" "$AE" build --emit=lib lib.ae -o "$TMP/liblib") > "$TMP/build.log" 2>&1; then
    echo "  [FAIL] emit_lib_result_types: ae build --emit=lib failed"
    sed 's/^/        /' "$TMP/build.log" | grep -m6 -E "error|warning"
    fail=1
fi

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "  [PASS] emit_lib_result_types: a result the ABI cannot carry skips its alias; a bit set crosses whole"
exit 0
