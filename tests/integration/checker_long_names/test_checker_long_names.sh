#!/bin/sh
# #3569: every name the checker builds is as long as it is. It joined
# qualified names into fixed 256- and 512-byte buffers, and snprintf cut a
# longer one to another name, which then named nothing: a module function,
# constant or alias past the buffer did not resolve from another file.
#
# Each name here is 600 characters, past every buffer the checker had:
#   call:      `lng.<name>()`, the qualified call (lookup_qualified_symbol).
#   tuple:     `a, b = lng.<name>()`, a qualified call whose result the
#              inference reads to type the destructured names.
#   const:     `lng.<NAME>`, a module constant read qualified.
#   selective: `import lng (<name>)` and a bare `<name>()`, the alias the
#              import binds and re-syncs after inference.
#   alias:     `import lng as <alias>` and `<alias>.<name>()`.
#   enum:      `<Enum>.<Member>` of an enum whose name is that long.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=checker_long_names

[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# A name of 600 characters starting with `$1`.
long() {
    awk -v p="$1" 'BEGIN { s = p; while (length(s) < 600) s = s "x"; print s }'
}
F=$(long f)
T=$(long t)
K=$(long K)
A=$(long a)
E=$(long E)

mkdir -p "$TMP/lib/lng"
cat > "$TMP/lib/lng/module.ae" <<AE
const $K = 9

$F() -> int {
    return 42
}

$T() -> (int, string) {
    return 7, "seven"
}
AE

cat > "$TMP/main.ae" <<AE
import lng
import lng ($F)
import lng as $A

enum $E { Red, Green }

main() {
    println("call \${lng.$F()}")
    n, s = lng.$T()
    println("tuple \${n} \${s}")
    println("const \${lng.$K}")
    println("selective \${$F()}")
    println("alias \${$A.$F()}")
    c = $E.Green
    if c == $E.Green { println("enum green") }
}
AE

out=$(cd "$TMP" && AETHER_HOME="$ROOT" "$AE" run main.ae 2>&1)
want="call 42
tuple 7 seven
const 9
selective 42
alias 42
enum green"
if [ "$(printf '%s\n' "$out" | tr -d '\r')" != "$want" ]; then
    echo "  [FAIL] $NAME: names past 256 and 512 bytes did not all resolve:"
    printf '%s\n' "$out" | cut -c1-200 | head -20 | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] $NAME: qualified, selective, aliased and enum names of 600 characters resolve"
exit 0
