#!/bin/sh
# #2162: a module's own struct must not collide with a type the runtime
# injects into the program's translation unit.
#
# `@c_include` (#1986) put std/collections/aether_arr_inline.h into the TU of
# every program importing std.intarr / std.floatarr / std.longarr, so that
# the element accessors inline. That header declared `struct IntArray`,
# `struct FloatArray` and `struct LongArray` -- bare, common names, now
# claimed inside someone else's program. aephysics had carried its own
# `struct IntArray` since its first layer; on 0.708.0 every program that
# imported both stopped compiling, with a C error naming neither module:
#
#   error: redefinition of struct or union 'struct IntArray'
#
# The rule this pins: a header the runtime injects declares only
# Aether-prefixed names. `aether_collections.h` keeps the short aliases,
# because C includes that one deliberately.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

[ -x "$AE" ] || { echo "  [SKIP] module_struct_name_collision: ae not built"; exit 0; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0
export AETHER_HOME="$ROOT"
mkdir -p "$tmp/lib/phys" "$tmp/proj"

# A module with its own IntArray -- the aephysics shape -- that also uses
# the packed arrays whose accessors are inlined through the header.
cat > "$tmp/lib/phys/module.ae" <<'AE'
import std.intarr
exports(IntArray, build_one, total)

struct IntArray {
    id: int
    weight: int
}

build_one(id: int, weight: int) -> IntArray {
    IntArray v
    v.id = id
    v.weight = weight
    return v
}

// ... while ALSO going through std.intarr, so both types are in the same
// translation unit and the collision is unavoidable if one exists.
total(n: int) -> int {
    a = intarr.intarr_new_raw(n)
    i = 0
    while i < n {
        intarr.intarr_set_unchecked(a, i, i * 2)
        i = i + 1
    }
    sum = 0
    i = 0
    while i < n {
        sum = sum + intarr.intarr_get_unchecked(a, i)
        i = i + 1
    }
    intarr.intarr_free(a)
    return sum
}
AE

cat > "$tmp/proj/main.ae" <<'AE'
import phys
main() {
    v = phys.build_one(7, 42)
    println("${v.id} ${v.weight} ${phys.total(5)}")
}
AE

out="$(cd "$tmp/proj" && "$AE" run --lib "$tmp/lib" main.ae 2>&1 | tail -1)"
if [ "$out" != "7 42 20" ]; then
    echo "  [FAIL] module_struct_name_collision: a module's own IntArray does not build beside std.intarr (got '$out')"
    (cd "$tmp/proj" && "$AE" run --lib "$tmp/lib" main.ae 2>&1 | grep -iE "redefinition|conflicting|error" | head -3 | sed 's/^/        /')
    fail=1
fi

# The rule itself, not just this one instance: nothing the injected header
# declares may be an unprefixed name. Checked against the header, so a name
# added later is caught before it reaches anyone's program.
hdr="$ROOT/std/collections/aether_arr_inline.h"
bad="$(grep -E '^(struct|typedef struct|typedef)' "$hdr" | grep -vE '\bAether[A-Za-z0-9_]+' || true)"
if [ -n "$bad" ]; then
    echo "  [FAIL] module_struct_name_collision: the injected header declares an unprefixed type"
    printf '%s\n' "$bad" | sed 's/^/        /'
    fail=1
fi

# And the two really are in one translation unit: the generated C declares
# the MODULE's IntArray and includes the runtime header in the same file.
# Without that, the build above could pass for the uninteresting reason
# that the header never arrived.
(cd "$tmp/proj" && "$AETHERC" --lib "$tmp/lib" main.ae out.c >/dev/null 2>&1)
if ! grep -q '#include "aether_arr_inline.h"' "$tmp/proj/out.c"; then
    echo "  [FAIL] module_struct_name_collision: the runtime header did not reach the TU, so the build proves nothing"
    fail=1
fi
if ! grep -q '} IntArray;' "$tmp/proj/out.c"; then
    echo "  [FAIL] module_struct_name_collision: the module's own IntArray is not in the generated C"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] module_struct_name_collision: a module's own IntArray builds beside std.intarr; the injected header claims only Aether-prefixed names"
fi
exit $fail
