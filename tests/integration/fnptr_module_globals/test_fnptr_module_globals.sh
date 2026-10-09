#!/bin/sh
# A module-level fn-pointer var or const that names an imported module's
# function (#2650, #2648).
#
# `var g_name: fn(string) -> string = strfns.lit` was refused "module
# 'strfns' has no export 'lit'", and once the module checks ran before the
# prune (#2613) it failed in gcc with 'strfns_lit' undeclared: the prune
# that drops an imported module's unreached functions never looked at a
# module-level initializer, so the one function only that initializer named
# was swept. Module-level vars and consts now seed the prune. A module's own
# fn-pointer const (#2648) is called through its typed pointer from the
# importer, `cbmod.CB(21)`, and the function only it names is kept.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] fnptr_module_globals: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
mkdir -p "$tmp/lib"

cat > "$tmp/lib/strfns.ae" <<'AE'
import std.string
exports(lit, ident, fresh)
lit(s: string) -> string {
    return "lit"
}
ident(s: string) -> string {
    return s
}
fresh(s: string) -> string {
    return string.concat(s, "+")
}
AE

cat > "$tmp/lib/cbmod.ae" <<'AE'
exports (CB, NAMED)
twice(n: int) -> int { return n * 2 }
named(n: int) -> string { return "named" }
const CB: fn(int) -> int = twice
const NAMED: fn(int) -> string = named
AE

cat > "$tmp/main.ae" <<'AE'
import std.string
import strfns
import cbmod

var g_name: fn(string) -> string = strfns.lit
const LOUD: fn(string) -> string = strfns.fresh

main() {
    w = string.concat("a", string.from_int(1))
    println(g_name(w))
    g_name = strfns.ident
    println(g_name(w))
    println(LOUD(w))
    println(cbmod.CB(21))
    println(cbmod.NAMED(0))
}
AE

out="$(cd "$tmp" && AETHER_HOME="$ROOT" "$AE" check main.ae 2>&1)"
if printf '%s' "$out" | grep -q "^error"; then
    echo "  [FAIL] fnptr_module_globals: ae check refused the module function in a global"
    printf '%s\n' "$out" | grep -E "^error|-->" | head -4 | sed 's/^/        /'
    exit 1
fi
got="$(cd "$tmp" && AETHER_HOME="$ROOT" "$AE" run main.ae 2>&1 | tr -d '\r' | tail -5 | tr '\n' ' ')"
if [ "$got" != "lit a1 a1+ 42 named " ]; then
    echo "  [FAIL] fnptr_module_globals: got '$got'"
    exit 1
fi
echo "  [PASS] fnptr_module_globals: a global's initializer keeps the module function it names; a module's fn-pointer const is called through its type"
exit 0
