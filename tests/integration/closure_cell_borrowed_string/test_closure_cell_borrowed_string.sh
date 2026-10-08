#!/bin/sh
# #2514: a store into a closure's string cell takes the value.
#
# A string a closure writes lives in a shared cell that owns what it
# holds. `s = p` inside the closure, `p` the caller's string the closure
# captured, stored `p` as it stood: the env held its own reference to `p`
# and the cell adopted the same pointer without one, so at scope exit both
# released it and the caller's string was freed while the caller still
# held it. The store now takes the value the way every owning slot does
# (a borrowed value copied or retained, a fresh one adopted); a match arm
# and a tuple destructure that bind such a variable store the same way.
# A function returning such a variable (`pick`) is heap-returning: the
# cell is released at its exit, so the caller gets a copy, not the cell's
# pointer (which printed freed memory once the cell owned its value).
# The macOS leaks gate runs tests/regression/test_closure_cell_borrowed_string.ae
# for the ownership; this checks the emitted store and the output.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] closure_cell_borrowed_string: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
import std.string

setit(p: string) -> int {
    s = "init"
    f = || { s = p }
    call(f)
    return string.length(s)
}

pick(p: string, n: int) -> string {
    s = "none"
    f = || {
        s = match n {
            1 -> p
            _ -> string.concat(p, "?")
        }
    }
    call(f)
    return s
}

two(p: string) -> (string, int) { return p, string.length(p) }

split(p: string) -> string {
    s = "x"
    k = 0
    f = || { s, k = two(p) }
    call(f)
    return "${s}/${k}"
}

main() {
    base = string.concat("ab", string.from_int(7))
    println("${setit(base)} ${base}")
    println("${setit(base)} ${base}")
    println("${pick(base, 1)} ${pick(base, 2)} ${base}")
    println("${split(base)} ${base}")
}
AE

want="3 ab7
3 ab7
ab7 ab7? ab7
ab7/3 ab7"
got="$("$AE" run "$tmp/main.ae" 2>&1 | tr -d '\r' | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] closure_cell_borrowed_string: the caller's string did not survive the closure's store"
    printf 'got:\n%s\nwant:\n%s\n' "$got" "$want" | sed 's/^/        /'
    exit 1
fi

# The cell adopted the captured parameter as it stood: `_aether_str_cell_set(s, p)`.
if ! "$AETHERC" "$tmp/main.ae" "$tmp/main.c" >/dev/null 2>&1; then
    echo "  [FAIL] closure_cell_borrowed_string: main.ae did not compile to C"
    exit 1
fi
if grep -q "_aether_str_cell_set(s, p)" "$tmp/main.c"; then
    echo "  [FAIL] closure_cell_borrowed_string: the string cell adopted the borrowed parameter without taking it"
    grep -n "_aether_str_cell_set(s, p)" "$tmp/main.c" | head -3 | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] closure_cell_borrowed_string: a store into a closure's string cell takes the value"
