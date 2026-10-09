#!/bin/sh
# #2618, #2369: what the ownership of string arrays and struct boxes costs in
# the generated C, and where it is not paid.
#
# A local `string[N]` array that some store makes own a string releases its
# elements at its scope exit; a table of literals must stay the plain C
# array it was, with no release and no copy on a read (the first attempt at
# #2618 ran such a lookup 2.1x slower). A call of C's malloc is a zeroing
# allocation, whether its pointer is cast to a struct at once or bound to a
# `ptr` first, so a field store can read the trackers of any box Aether
# allocated (#2369).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
[ -f "$AETHERC.exe" ] && AETHERC="$AETHERC.exe"

if [ ! -f "$AETHERC" ]; then
    echo "  [SKIP] string_array_ownership_codegen: $AETHERC not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
import std.string

@extern("malloc") c_malloc(n: int) -> ptr

struct Named {
    name: string
}

struct Plain {
    n: int
}

table_lookup(i: int) -> string {
    t = ["zero", "one", "two", "three", "four", "five", "six", "seven"]
    return t[i]
}

owning_lookup(w: string, i: int) -> string {
    t = [string.concat(w, "0"), "one"]
    return t[i]
}

boxes() -> int {
    a = c_malloc(sizeof(Named)) as *Named
    a.name = "x"
    raw = c_malloc(sizeof(Named))
    b = raw as *Named
    b.name = "y"
    c = c_malloc(sizeof(Plain)) as *Plain
    c.n = 1
    return c.n
}

main() {
    println(table_lookup(1))
    println(owning_lookup("w", 0))
    println("${boxes()}")
}
AE

fail() {
    echo "  [FAIL] string_array_ownership_codegen: $1"
    exit 1
}

if ! "$AETHERC" "$tmp/main.ae" "$tmp/main.c" >"$tmp/aetherc.log" 2>&1; then
    sed 's/^/    /' "$tmp/aetherc.log" | head -20
    fail "aetherc failed"
fi

# The body of one generated function, from its definition to the closing brace.
body() {
    awk -v sig="$1" 'index($0, sig) == 1 && /\{$/ { on = 1 } on { print } on && /^}/ { exit }' "$tmp/main.c"
}

body "const char* table_lookup(" > "$tmp/table.c"
body "const char* owning_lookup(" > "$tmp/owning.c"
body "int boxes(" > "$tmp/boxes.c"
[ -s "$tmp/table.c" ] && [ -s "$tmp/owning.c" ] && [ -s "$tmp/boxes.c" ] ||
    fail "generated functions not found"

if grep -q "_aether_str_cell\|aether_uniform_heap_str" "$tmp/table.c"; then
    sed 's/^/    /' "$tmp/table.c"
    fail "a table of literals pays for ownership"
fi
grep -q "_aether_str_cell_free_val(t\[" "$tmp/owning.c" ||
    fail "an owning array is not released at its exit"
grep -q "aether_uniform_heap_str((const char\*)(t\[" "$tmp/owning.c" ||
    fail "an owning array's element is returned without a copy"
grep -q "((Named\*)(calloc(1, " "$tmp/boxes.c" ||
    fail "malloc(n) as *Named is not a zeroing allocation"
grep -q "raw = calloc(1, " "$tmp/boxes.c" ||
    fail "a malloc bound to a ptr is not a zeroing allocation"
if grep -q "malloc(" "$tmp/boxes.c"; then
    sed 's/^/    /' "$tmp/boxes.c"
    fail "a call of malloc is left as it was"
fi

echo "  [PASS] string_array_ownership_codegen: literal tables stay plain; owning arrays release; malloc zeroes"
