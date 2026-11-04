#!/bin/sh
# #3574: the code generator follows everything the program has. Each case
# passed a fixed array's size, and past it the build named the wrong thing,
# dropped it, or took it for none, without a word:
#
#   builder_release: a builder made by a factory of a 520-character stem,
#                    its block left by a `return`: the release the way out
#                    calls was read through 512 bytes, and the C named a
#                    function that is not there.
#   builder_depth:   a builder call recursing inside its own block, 100
#                    deep: the blocks' stack held 64, the push of a deeper
#                    block was dropped, and its calls set an outer block's
#                    config.
#   wide_fnptr:      a C function pointer type (`cfn`) of 300 parameters: its
#                    C spelling was cut at 1024 bytes, and the C did not compile.
#   wide_export:     an --emit=lib export of 40 parameters: 32 slots cost it
#                    its alias, with a warning that a parameter type is not
#                    representable.
#   lib_id:          a library id of 200 characters: the symbols named after
#                    it were cut at 127, so two libraries whose ids share
#                    their start defined one symbol.
#   string_struct:   an export taking a pointer to a struct of a
#                    250-character name with a string field: the spelling was
#                    read through 200 bytes, the struct was not found, and the
#                    catalog did not name the library's own runtime.
#   deep_struct:     the same through a chain of 70 structs: the walk went 64
#                    deep and took the string at the end for none.
#   lane_name:       a function of a 600-character name in a module imported
#                    at @target, taking an f32x8, called from the build's own
#                    code: its name was read through 512 bytes, the callee
#                    was not found, and the crossing was not refused.
#   lane_deep:       a pointer to a struct holding an f32x8 30 structs down,
#                    taken by the @target copy: the walk went 24 deep and the
#                    crossing was not refused.
#   lane_structs:    a module imported at @target whose functions use 520
#                    structs holding lanes: 512 were remapped to the copy's
#                    own, and the rest kept the build's, so the C did not
#                    compile.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
NAME=codegen_no_silent_caps

[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fails=0

fail() {
    echo "  [FAIL] $NAME: $1"
    [ -n "${2:-}" ] && printf '%s\n' "$2" | cut -c1-160 | head -8 | sed 's/^/        /'
    fails=$((fails + 1))
}

# runs <dir> <want>: `ae run` of $TMP/<dir>/main.ae prints <want>.
runs() {
    out=$(cd "$TMP/$1" && AETHER_HOME="$ROOT" "$AE" run main.ae 2>&1 | tr -d '\r')
    [ "$out" = "$2" ] || fail "$1: not \"$2\":" "$out"
}

# refused <dir> <text>: `ae build` of $TMP/<dir>/main.ae fails, saying <text>.
refused() {
    out=$(cd "$TMP/$1" && AETHER_HOME="$ROOT" "$AE" build main.ae -o "$TMP/$1/main" 2>&1)
    if [ $? -eq 0 ] || ! printf '%s' "$out" | grep -qF "$2"; then
        fail "$1: not refused with \"$2\":" "$out"
    fi
}

long() {  # long <prefix> <length>
    awk -v p="$1" -v n="$2" 'BEGIN { s = p; while (length(s) < n) s = s "x"; print s }'
}

# --- builder_release ----------------------------------------------------------
mkdir -p "$TMP/builder_release"
STEM=$(long o 520)
cat > "$TMP/builder_release/main.ae" <<AE
import std.map

${STEM}_new() -> ptr {
    return map_new()
}

${STEM}_free(m: ptr) {
    map_free(m)
}

builder tuned(label: string) with ${STEM}_new {
    if _builder != null {
        ${STEM}_free(_builder)
    }
}

set_opt(_ctx: ptr, key: string, value: string) {
    map_put(_ctx, key, value)
}

early(n: int) -> int {
    tuned("x") {
        set_opt("k", "v")
        if n > 0 {
            return n
        }
    }
    return 0
}

main() {
    println("early \${early(7)}")
}
AE
runs builder_release "early 7"

# --- builder_depth ------------------------------------------------------------
mkdir -p "$TMP/builder_depth"
cat > "$TMP/builder_depth/main.ae" <<'AE'
import std.map

var g_wrong = 0

builder node(depth: int) {
    if _builder != null {
        if map_size(_builder) != 1 {
            g_wrong = g_wrong + 1
        }
        map_free(_builder)
    }
}

set_opt(_ctx: ptr, key: string, value: string) {
    map_put(_ctx, key, value)
}

walk(d: int) {
    node(d) {
        set_opt("k", "v")
        if d < 100 {
            walk(d + 1)
        }
    }
}

main() {
    walk(1)
    println("wrong ${g_wrong}")
}
AE
runs builder_depth "wrong 0"

# --- wide_fnptr ---------------------------------------------------------------
mkdir -p "$TMP/wide_fnptr"
awk 'BEGIN {
    printf "cfn Wide("
    for (i = 0; i < 300; i++) printf "%sa%d: int", (i ? ", " : ""), i
    print ") -> int"
    printf "sum("
    for (i = 0; i < 300; i++) printf "%sa%d: int", (i ? ", " : ""), i
    print ") -> int { return a0 + a299 }"
    printf "call_it(f: Wide) -> int { return f("
    for (i = 0; i < 300; i++) printf "%s%d", (i ? ", " : ""), (i == 0 ? 1 : (i == 299 ? 2 : 0))
    print ") }"
    print "main() { println(\"sum ${call_it(sum)}\") }"
}' > "$TMP/wide_fnptr/main.ae"
runs wide_fnptr "sum 3"

# --- wide_export --------------------------------------------------------------
mkdir -p "$TMP/wide_export"
awk 'BEGIN {
    printf "wide("
    for (i = 0; i < 40; i++) printf "%sp%d: int", (i ? ", " : ""), i
    print ") -> int { return p0 + p39 }"
}' > "$TMP/wide_export/lib.ae"
out=$(cd "$TMP/wide_export" && AETHER_HOME="" "$AETHERC" --emit=lib lib.ae "$TMP/wide_export/lib.c" 2>&1)
if printf '%s' "$out" | grep -q "isn't representable"; then
    fail "wide_export: the export was taken for one of a type the ABI does not carry:" "$out"
fi
if ! grep -q "int32_t aether_wide(.*int32_t p39)" "$TMP/wide_export/lib.c" 2>/dev/null; then
    fail "wide_export: no alias aether_wide taking p0 to p39" "$(grep -n 'aether_wide' "$TMP/wide_export/lib.c" 2>/dev/null | head -3)"
fi

# --- lib_id -------------------------------------------------------------------
mkdir -p "$TMP/lib_id"
echo 'main() {}' > "$TMP/lib_id/lib.ae"
ID=$(long l 200)
got=$(cd "$TMP/lib_id" && AETHER_HOME="" "$AETHERC" "--lib-id=$ID" --print-string-release lib.ae "$TMP/lib_id/lib.c" 2>&1 | tr -d '\r')
[ "$got" = "aether_${ID}_string_release" ] || fail "lib_id: the symbol is not the whole id's:" "$got"

# --- string_struct, deep_struct -----------------------------------------------
# own_runtime <dir>: the catalog of `ae build --emit=csrc` of lib.ae names the
# library's own runtime among what an importer must do.
own_runtime() {
    out=$(cd "$TMP/$1" && AETHER_HOME="$ROOT" "$AE" build --emit=csrc lib.ae -o "$TMP/$1/lib" 2>&1)
    json="$TMP/$1/lib.catalog.json"
    if [ ! -f "$json" ]; then
        fail "$1: no catalog:" "$out"
    elif ! tr -d '\r' < "$json" | grep '"requires"' | grep -q '"own_runtime"'; then
        fail "$1: the catalog does not name own_runtime:" "$(grep '"requires"' "$json")"
    fi
}

mkdir -p "$TMP/string_struct"
SNAME=$(long S 250)
cat > "$TMP/string_struct/lib.ae" <<AE
struct $SNAME {
    name: string
    n: int
}

exports (size_of, $SNAME)

size_of(p: *$SNAME) -> int {
    return p.n
}
AE
own_runtime string_struct

mkdir -p "$TMP/deep_struct"
awk 'BEGIN {
    for (i = 0; i < 70; i++) {
        printf "struct D%d {\n", i
        if (i < 69) printf "    next: *D%d\n", i + 1
        else print "    name: string"
        print "    n: int"
        print "}"
        print ""
    }
    printf "exports (size_of"
    for (i = 0; i < 70; i++) printf ", D%d", i
    print ")"
    print ""
    print "size_of(p: *D0) -> int {"
    print "    return p.n"
    print "}"
}' > "$TMP/deep_struct/lib.ae"
own_runtime deep_struct

# --- lane_name ----------------------------------------------------------------
mkdir -p "$TMP/lane_name"
LANE=$(long t 600)
cat > "$TMP/lane_name/twice.ae" <<AE
import std.lanes
exports ($LANE)
$LANE(v: f32x8) -> f32x8 { return v + v }
AE
cat > "$TMP/lane_name/main.ae" <<AE
import std.lanes
@target("avx2") import twice as t8
main() { println("\${lanes.sum8(t8.$LANE(lanes.splat8(1.0)))}") }
AE
refused lane_name "error[E0200]: 'twice__avx2.txxxxxxxx"

# --- lane_deep ----------------------------------------------------------------
mkdir -p "$TMP/lane_deep"
awk 'BEGIN {
    print "import std.lanes"
    printf "exports (zero"
    for (i = 0; i < 30; i++) printf ", L%d", i
    print ")"
    for (i = 0; i < 30; i++) {
        printf "struct L%d {\n", i
        if (i < 29) printf "    inner: L%d\n", i + 1
        else print "    x: f32x8"
        print "}"
    }
    print "zero(p: *L0) {}"
}' > "$TMP/lane_deep/deep.ae"
cat > "$TMP/lane_deep/main.ae" <<'AE'
import std.lanes
@target("avx2") import deep as d8
main() {
    d8.zero(null)
    println("zeroed")
}
AE
refused lane_deep "passes eight-lane values between a module imported at @target(\"avx2\") and the build's own code"

# --- lane_structs -------------------------------------------------------------
# The copy's functions are compiled when something reaches them: count()
# names each, behind a test that is false when the program runs.
mkdir -p "$TMP/lane_structs"
awk 'BEGIN {
    print "import std.lanes"
    print "exports (count)"
    print "var g_never = 0"
    for (i = 0; i < 520; i++) {
        printf "struct S%d {\n    x: f32x8\n}\n", i
        printf "touch%d(p: *S%d) {\n    p.x = lanes.splat8(1.0)\n}\n", i, i
    }
    print "count() -> int {"
    print "    if g_never == 1 {"
    for (i = 0; i < 520; i++) printf "        touch%d(null)\n", i
    print "    }"
    print "    return 520"
    print "}"
}' > "$TMP/lane_structs/many.ae"
cat > "$TMP/lane_structs/main.ae" <<'AE'
import many
@target("avx2") import many as m8
main() { println("structs ${m8.count()} ${many.count()}") }
AE
runs lane_structs "structs 520 520"

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] $NAME: builder releases and depth, fn pointers, exports, library ids, catalog strings, lane crossings and lane structs past every old cap"
exit 0
