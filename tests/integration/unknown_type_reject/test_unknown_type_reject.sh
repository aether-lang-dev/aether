#!/bin/sh
# Regression (#2896): a type name that names no type of the program is an
# Aether error (E0302), never C.
#
# `while i < 2 and eat(i) { ... }` passed aetherc: Aether's logical AND is
# `&&`, so `and eat` read as a C-style declaration of `eat` of a type `and`
# (`IDENT IDENT`), the brace-less loop body, and the C compiler then said
# "unknown type name 'and'". Any such name did the same, as a local, a
# parameter or a struct field. The type checker refuses each now, and for
# `and` and `not` the help names the operator Aether spells. Every kind of
# type the program declares is still a type: a struct, an enum, a sum, a
# bitstruct, a distinct type, a cfn type, an actor and a C overlay, as a
# local, a parameter and a field.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
EXE=""
[ -x "$ROOT/build/ae" ] || EXE=".exe"
AE="$ROOT/build/ae$EXE"
AETHERC="$ROOT/build/aetherc$EXE"
if [ ! -x "$AE" ] || [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] unknown_type_reject: compiler not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0

# refused <name> <message part> <help part>
refused() {
    if "$AETHERC" "$TMP/$1.ae" "$TMP/$1.c" > "$TMP/$1.log" 2>&1; then
        echo "  [FAIL] unknown_type_reject: $1 compiled (should be refused)"
        fail=1
        return
    fi
    if ! grep -q "error\[E0302\]: $2" "$TMP/$1.log" || ! grep -qF "$3" "$TMP/$1.log"; then
        echo "  [FAIL] unknown_type_reject: $1 not refused with E0302 '$2' and help '$3'"
        grep -m3 -E "error|help" "$TMP/$1.log" | cut -c1-200 | sed 's/^/        /'
        fail=1
    fi
}

cat > "$TMP/and_while.ae" <<'AE'
eat(n: int) -> bool {
    return n >= 0
}

main() {
    i = 0
    while i < 2 and eat(i) {
        i = i + 1
    }
    println("${i}")
}
AE
refused and_while "Unknown type 'and': the local 'eat'" "Aether's logical AND is \`&&\`"

cat > "$TMP/not_if.ae" <<'AE'
main() {
    done = false
    if done {
        println("x")
    }
    not done
}
AE
refused not_if "Unknown type 'not': the local 'done'" "Aether's logical NOT is \`!\`"

cat > "$TMP/local.ae" <<'AE'
main() {
    Shape s
    println("x")
}
AE
refused local "Unknown type 'Shape': the local 's'" "declare the struct, enum or sum"

cat > "$TMP/param.ae" <<'AE'
area(s: Shape) -> int {
    return 1
}

main() {
    println("x")
}
AE
refused param "Unknown type 'Shape': the parameter 's'" "declare the struct, enum or sum"

cat > "$TMP/field.ae" <<'AE'
struct Scene {
    first: Shape
}

main() {
    println("x")
}
AE
refused field "Unknown type 'Shape': the field 'first'" "declare the struct, enum or sum"

cat > "$TMP/array_param.ae" <<'AE'
total(xs: Shape[2]) -> int {
    return 2
}

main() {
    println("x")
}
AE
refused array_param "Unknown type 'Shape': the parameter 'xs'" "declare the struct, enum or sum"

# Every kind of declared type is a type, as a local, a parameter, a field
# and an array element.
cat > "$TMP/known.ae" <<'AE'
extern malloc(size: int) -> ptr

struct Point {
    x: int
    y: int
}

enum Color { Red, Green }

type Meters = distinct int

bitstruct Flags : uint8_t {
    on: bool 0
}

cfn Callback(n: int) -> int

message Ping {
    n: int
}

struct Holder {
    p: Point
    c: Color
    m: Meters
    f: Flags
    cb: Callback
    ps: Point[2]
}

twice(n: int) -> int {
    return n * 2
}

use(p: Point, c: Color, m: Meters, h: Holder, q: ptr) -> int {
    return p.x + h.p.y
}

main() {
    Point p
    p.x = 3
    p.y = 4
    Color c = Red
    Meters m = 5 as Meters
    Flags f
    Holder h
    h.p = p
    Point[2] ps = [p, p]
    println("${use(p, c, m, h, null)} ${ps[1].y}")
}
AE
if ! ( cd "$TMP" && "$AE" build known.ae -o "$TMP/known" ) > "$TMP/known.log" 2>&1; then
    echo "  [FAIL] unknown_type_reject: declared types refused as unknown"
    grep -m5 "error" "$TMP/known.log" | cut -c1-200 | sed 's/^/        /'
    fail=1
else
    got="$("$TMP/known$EXE" 2>&1 | tr -d '\r')"
    if [ "$got" != "7 4" ]; then
        echo "  [FAIL] unknown_type_reject: the declared-types program printed '$got', not '7 4'"
        fail=1
    fi
fi

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "  [PASS] unknown_type_reject: a type name that names nothing is refused (E0302), and and not name the operators; declared types are types"
exit 0
