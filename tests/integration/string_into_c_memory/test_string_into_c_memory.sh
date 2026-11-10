#!/bin/sh
# A string stored into C memory viewed as `string[]` is its characters; the
# memory owns nothing (#2979, R4). E0902 refused a string built in the
# statement and its help said to bind it to a local first, which compiled
# and freed the string when the function returned: the array then held a
# freed string. A local (or any string an owner frees) stored into C memory
# that outlives the function is now E0902 too: memory reached through a
# parameter, a module-level `var`, a field or a heap box, or whose pointer
# is returned or stored. The help names `string.keep(s)` through a `ptr[]`
# view and `string.release_kept` where the memory is freed. Memory the
# block allocates and does not let escape (an argv array handed to C) still
# takes a local, and the help for a string built in the statement says
# which form fits which case. A literal, or a `const` bound to one, is
# pinned and fits both. prog.ae runs both forms, as it is and under the
# poisoning allocator, and checks the keeps are given back.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"
[ -x "$AE" ] || { echo "  [SKIP] string_into_c_memory: ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fails=0

# refused <name> <line:col> <message> <help>: `ae check` refuses <name>.ae
# with E0902 at that position, with that message and help.
refused() {
    out=$(cd "$TMP" && AETHER_HOME="$ROOT" "$AE" check "$1.ae" 2>&1)
    case "$out" in
        *"error[E0902]: $3"*"$1.ae:$2"*"$4"*) ;;
        *) echo "  [FAIL] string_into_c_memory: $1 is not E0902 \"$3\" at $2 with help \"$4\":"
           echo "$out" | head -8 | sed 's/^/        /'
           fails=$((fails + 1)) ;;
    esac
}

LOCAL="a string is stored into C memory that outlives the function"
BUILT_OUT="a string built here is stored into C memory that outlives the function"
BUILT_HERE="a string built here is stored into C memory, which owns nothing"
KEEP='store `string.keep(s)` through a `ptr[]` view, `(p as ptr[])[i] = string.keep(s)`, and give it back with `string.release_kept(q)` where the memory is freed'

cat > "$TMP/param.ae" <<'AE'
import std.string

extern calloc(n: int, size: int) -> ptr

fill(names: ptr, name: string) {
    kept = string.copy(name)
    (names as string[])[0] = kept
}

main() { fill(calloc(1, 8), "x") }
AE
refused param 7:30 "$LOCAL" "$KEEP"

cat > "$TMP/global.ae" <<'AE'
import std.string

var names: ptr = null

fill(name: string) {
    kept = string.copy(name)
    (names as string[])[0] = kept
}

main() { fill("x") }
AE
refused global 7:30 "$LOCAL" "$KEEP"

cat > "$TMP/box.ae" <<'AE'
import std.string

extern calloc(n: int, size: int) -> ptr

struct Anims {
    names: ptr
}

fill(a: *Anims, name: string) {
    kept = string.copy(name)
    (a.names as string[])[0] = kept
}

main() {
    a = heap.new(Anims)
    a.names = calloc(1, 8)
    fill(a, "x")
}
AE
refused box 11:32 "$LOCAL" "$KEEP"

cat > "$TMP/returned.ae" <<'AE'
import std.string

extern calloc(n: int, size: int) -> ptr

build_names(name: string) -> ptr {
    names = calloc(1, 8)
    (names as string[])[0] = name
    return names
}

main() { p = build_names("x") }
AE
refused returned 7:30 "$LOCAL" "$KEEP"

cat > "$TMP/built_out.ae" <<'AE'
import std.string

fill(names: ptr, name: string) {
    (names as string[])[0] = string.copy(name)
}

main() { fill(null, "x") }
AE
refused built_out 4:36 "$BUILT_OUT" "$KEEP"

cat > "$TMP/built_here.ae" <<'AE'
import std.string

extern calloc(n: int, size: int) -> ptr
extern free(p: ptr)

run(name: string) {
    argv = calloc(2, 8)
    (argv as string[])[0] = string.copy(name)
    free(argv)
}

main() { run("x") }
AE
refused built_here 8:35 "$BUILT_HERE" "if C's use of the memory ends in this block, bind the string to a local here and store the local; if the memory outlives the function, $KEEP"

# A local into block-local memory, and string.keep into memory that outlives
# the function: built, run, and run again under the poisoning allocator.
if ! (cd "$TMP" && AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/prog.ae" -o "$TMP/prog") > "$TMP/build.log" 2>&1; then
    echo "  [FAIL] string_into_c_memory: prog.ae did not build"
    sed 's/^/        /' "$TMP/build.log" | head -8
    fails=$((fails + 1))
else
    out="$("$TMP/prog" 2>&1)"
    if [ "$out" != "ok" ]; then
        echo "  [FAIL] string_into_c_memory: prog.ae printed: $out"
        fails=$((fails + 1))
    fi
fi
# The poisoning allocator needs the size of the block it poisons. macOS's
# linker has no --wrap, and OpenBSD's libc has no malloc_usable_size (or any
# other way to ask a block's size), so neither can build poison_alloc.c.
case "$(uname -s)" in
    Darwin|OpenBSD) echo "    skip: poisoning allocator (needs ld --wrap and malloc_usable_size)" ;;
    *)
    WRAP="-Wl,--wrap=free,--wrap=realloc,--wrap=aether_numa_free,--wrap=aether_numa_free_aligned"
    if (cd "$TMP" && AETHER_HOME="$ROOT" AE_CC="${AE_CC:-${CC:-cc}} $WRAP" "$AE" build "$SCRIPT_DIR/prog.ae" \
            --extra "$ROOT/tests/ownership_grid/poison_alloc.c" -o "$TMP/poisoned") > "$TMP/pbuild.log" 2>&1; then
        out="$(OWNERSHIP_GRID_POISON=1 "$TMP/poisoned" 2>&1)"
        if [ "$out" != "ok" ]; then
            echo "  [FAIL] string_into_c_memory: prog.ae under the poisoning allocator printed: $out"
            fails=$((fails + 1))
        fi
    else
        echo "  [FAIL] string_into_c_memory: prog.ae did not build with the poisoning allocator"
        sed 's/^/        /' "$TMP/pbuild.log" | head -8
        fails=$((fails + 1))
    fi
    ;;
esac

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] string_into_c_memory: a string into C memory that outlives the function goes through string.keep; block-local memory takes a local"
