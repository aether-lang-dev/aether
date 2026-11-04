#!/bin/sh
# #3574: nothing in the checker or the parser cuts or drops silently. Each
# case below passed a fixed array's size, and the program past it was
# compiled wrong: checked against what fit, the rest dropped without a word.
#
#   enums:       300 enums; the registry held 256, and the last ones' members
#                resolved to nothing.
#   distinct:    300 distinct types; the resolver held 256, and the last one
#                stayed the parser's placeholder struct.
#   cfn:         300 named C function-pointer types, the same.
#   sums:        300 sum types, the same.
#   owned:       65 owned pointer types; 64 were registered, and the 65th was
#                a plain `ptr`, so a second holder without `move` compiled.
#   arms:        an actor with 260 receive arms and a duplicate of the last:
#                the duplicate check recorded 256 and missed it.
#   members:     an enum of 70 members and a match without the last: the
#                exhaustiveness check tracked 64 and compiled it.
#   variants:    a sum of 70 variants, the same.
#   keys:        a match of 300 int arms repeating the last: the
#                unreachable-arm check kept 256 keys and missed it.
#   selector:    a comma-list arm of 300 enum members, the last no member:
#                256 were checked.
#   defers:      a block of 70 defers, the last moving an isolated value the
#                block moves and binds again after them: 64 were held for
#                the end of the block, and the 70th, checked where it is
#                written, took the move after it for a use of a moved value.
#   isolated:    130 isolated values moved, the last used after: the move
#                set held 128.
#   fn_params:   a function of 130 `fn` parameters keeping the last in a
#                module var: 128 were tracked, and the last kept nothing.
#   keep_modes:  a function of 70 `ptr` parameters, the last `@keep`, given
#                to a bare `fn`: its type kept 64 modes and borrowed the last.
#   type_modes:  a closure type of 70 `ptr` parameters, the last `@keep`, the
#                same in the parser.
#   places:      a destructure into 70 places: the parser refused the 65th
#                as no destructure slot.
#   overlays:    300 @c_struct overlays; 256 were registered, and a cast to
#                the last was refused.
#   late_binding: a view bound after 300 statements and freed: the walk
#                that finds a local's binding kept 256 nodes pending and did
#                not find it, so `free` of the view compiled.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=checker_no_silent_caps

[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fails=0

# runs <case> <want>: `ae run` of $TMP/<case>.ae prints <want>.
runs() {
    out=$(cd "$TMP" && AETHER_HOME="$ROOT" "$AE" run "$1.ae" 2>&1 | tr -d '\r')
    if [ "$out" != "$2" ]; then
        echo "  [FAIL] $NAME: $1: not \"$2\":"
        printf '%s\n' "$out" | cut -c1-160 | head -8 | sed 's/^/        /'
        fails=$((fails + 1))
    fi
}

# refused <case> <text>: `ae check` of $TMP/<case>.ae fails, saying <text>.
refused() {
    out=$(cd "$TMP" && AETHER_HOME="$ROOT" "$AE" check "$1.ae" 2>&1)
    rc=$?
    if [ $rc -eq 0 ] || ! printf '%s' "$out" | grep -qF "$2"; then
        echo "  [FAIL] $NAME: $1: not refused with \"$2\":"
        printf '%s\n' "$out" | cut -c1-160 | head -8 | sed 's/^/        /'
        fails=$((fails + 1))
    fi
}

# accepted <case>: `ae check` of $TMP/<case>.ae passes.
accepted() {
    out=$(cd "$TMP" && AETHER_HOME="$ROOT" "$AE" check "$1.ae" 2>&1)
    if [ $? -ne 0 ]; then
        echo "  [FAIL] $NAME: $1: refused:"
        printf '%s\n' "$out" | cut -c1-160 | head -8 | sed 's/^/        /'
        fails=$((fails + 1))
    fi
}

# warned <case> <text>: `ae check` of $TMP/<case>.ae passes with <text>.
warned() {
    out=$(cd "$TMP" && AETHER_HOME="$ROOT" "$AE" check "$1.ae" 2>&1)
    if ! printf '%s' "$out" | grep -qF "$2"; then
        echo "  [FAIL] $NAME: $1: no \"$2\":"
        printf '%s\n' "$out" | cut -c1-160 | head -8 | sed 's/^/        /'
        fails=$((fails + 1))
    fi
}

awk 'BEGIN {
    for (i = 0; i < 300; i++) printf "enum E%d { A%d, B%d }\n", i, i, i
    print "main() {"
    print "    c = E299.B299"
    print "    if c == E299.B299 { println(\"enum ok\") }"
    print "}"
}' > "$TMP/enums.ae"
runs enums "enum ok"

awk 'BEGIN {
    for (i = 0; i < 300; i++) printf "type D%d = distinct int\n", i
    print "value(d: D299) -> int { return d as int }"
    print "main() {"
    print "    println(\"distinct ${value(7 as D299)}\")"
    print "}"
}' > "$TMP/distinct.ae"
runs distinct "distinct 7"

awk 'BEGIN {
    for (i = 0; i < 300; i++) printf "cfn F%d(a: int) -> int\n", i
    print "takes(f: F299) -> int { return 1 }"
    print "main() {"
    print "    println(\"cfn ${takes(null)}\")"
    print "}"
}' > "$TMP/cfn.ae"
runs cfn "cfn 1"

awk 'BEGIN {
    print "struct A { n: int }"
    print "struct B { n: int }"
    for (i = 0; i < 300; i++) printf "type S%d = A | B\n", i
    print "pick(s: S299) -> int {"
    print "    return match s {"
    print "        A -> 1"
    print "        B -> 2"
    print "    }"
    print "}"
    print "main() {"
    print "    println(\"sum ${pick(B { n: 0 })}\")"
    print "}"
}' > "$TMP/sums.ae"
runs sums "sum 2"

awk 'BEGIN {
    print "import std.bytes"
    for (i = 0; i < 65; i++) printf "type O%d = owned ptr drop bytes.free\n", i
    print "main() {"
    print "    b = bytes.new(4) as O64"
    print "    c = b"
    print "}"
}' > "$TMP/owned.ae"
refused owned "E0905"

awk 'BEGIN {
    for (i = 0; i < 260; i++) printf "message M%d {}\n", i
    print "actor A {"
    print "    receive {"
    for (i = 0; i < 260; i++) printf "        M%d() -> {}\n", i
    print "        M259() -> {}"
    print "    }"
    print "}"
    print "main() {"
    print "    a = spawn(A())"
    print "    a ! M0 {}"
    print "}"
}' > "$TMP/arms.ae"
refused arms "this receive arm for 'M259' can never run"

awk 'BEGIN {
    printf "enum Big {"
    for (i = 0; i < 70; i++) printf "%s V%d", (i ? "," : ""), i
    print " }"
    print "name(b: Big) -> int {"
    print "    return match b {"
    for (i = 0; i < 69; i++) printf "        V%d -> %d\n", i, i
    print "    }"
    print "}"
    print "main() { println(\"${name(Big.V0)}\") }"
}' > "$TMP/members.ae"
refused members "non-exhaustive match on enum 'Big': missing member V69"

awk 'BEGIN {
    for (i = 0; i < 70; i++) printf "struct W%d { n: int }\n", i
    printf "type Wide ="
    for (i = 0; i < 70; i++) printf "%s W%d", (i ? " |" : ""), i
    print ""
    print "pick(w: Wide) -> int {"
    print "    return match w {"
    for (i = 0; i < 69; i++) printf "        W%d -> %d\n", i, i
    print "    }"
    print "}"
    print "main() { println(\"${pick(W0 { n: 0 })}\") }"
}' > "$TMP/variants.ae"
refused variants "non-exhaustive match on sum type 'Wide': missing variant W69"

awk 'BEGIN {
    print "pick(n: int) -> int {"
    print "    return match n {"
    for (i = 0; i < 300; i++) printf "        %d -> %d\n", i, i
    print "        299 -> 1"
    print "        _ -> 0"
    print "    }"
    print "}"
    print "main() { println(\"${pick(3)}\") }"
}' > "$TMP/keys.ae"
warned keys "this case is already handled on line"

awk 'BEGIN {
    printf "enum Big {"
    for (i = 0; i < 300; i++) printf "%s V%d", (i ? "," : ""), i
    print " }"
    print "pick(b: Big) -> int {"
    print "    return match b {"
    printf "        "
    for (i = 0; i < 300; i++) printf "V%d, ", i
    print "Nope -> 1"
    print "    }"
    print "}"
    print "main() { println(\"${pick(Big.V0)}\") }"
}' > "$TMP/selector.ae"
refused selector "'Nope' is not a member of enum 'Big'"

awk 'BEGIN {
    print "import std.bytes"
    print "keep(b: Isolated[ptr]) {}"
    print "main() {"
    print "    x = isolate(bytes.new(4))"
    print "    {"
    for (i = 0; i < 69; i++) printf "        defer println(\"d%d\")\n", i
    print "        defer keep(x)"
    print "        keep(x)"
    print "        x = isolate(bytes.new(4))"
    print "    }"
    print "}"
}' > "$TMP/defers.ae"
accepted defers

awk 'BEGIN {
    print "import std.bytes"
    print "keep(b: Isolated[ptr]) {}"
    print "main() {"
    for (i = 0; i < 130; i++) printf "    x%d = isolate(bytes.new(4))\n", i
    for (i = 0; i < 130; i++) printf "    keep(x%d)\n", i
    print "    keep(x129)"
    print "}"
}' > "$TMP/isolated.ae"
refused isolated "use of moved value 'x129'"

awk 'BEGIN {
    print "var kept: fn = null"
    printf "many("
    for (i = 0; i < 130; i++) printf "%sp%d: fn", (i ? ", " : ""), i
    print ") {"
    print "    kept = p129"
    print "}"
    print "main() { println(\"x\") }"
}' > "$TMP/fn_params.ae"
refused fn_params "E0902"

awk 'BEGIN {
    printf "wide("
    for (i = 0; i < 69; i++) printf "%sp%d: ptr", (i ? ", " : ""), i
    print ", p69: @keep ptr) {}"
    print "main() {"
    print "    let f: fn = wide"
    print "}"
}' > "$TMP/keep_modes.ae"
refused keep_modes "E0902"

awk 'BEGIN {
    printf "run(f: fn("
    for (i = 0; i < 69; i++) printf "%sptr", (i ? ", " : "")
    print ", @keep ptr), p: ptr) {"
    printf "    f("
    for (i = 0; i < 69; i++) printf "null, "
    print "p)"
    print "}"
    print "main() { println(\"x\") }"
}' > "$TMP/type_modes.ae"
refused type_modes "E0902"

awk 'BEGIN {
    print "struct P {"
    for (i = 0; i < 70; i++) printf "    f%d: int\n", i
    print "}"
    print "many() -> ("
    for (i = 0; i < 70; i++) printf "%sint", (i ? ", " : "")
    print ") {"
    printf "    return "
    for (i = 0; i < 70; i++) printf "%s%d", (i ? ", " : ""), i
    print ""
    print "}"
    print "main() {"
    print "    p = P {}"
    printf "    "
    for (i = 0; i < 70; i++) printf "%sp.f%d", (i ? ", " : ""), i
    print " = many()"
    print "    println(\"places ${p.f69}\")"
    print "}"
}' > "$TMP/places.ae"
runs places "places 69"

awk 'BEGIN {
    print "extern malloc(n: int) -> ptr"
    print "extern free(p: ptr)"
    for (i = 0; i < 300; i++) printf "@c_struct C%d {\n    x: int @0\n}\n", i
    print "main() {"
    print "    buf = malloc(16)"
    print "    p = buf as *C299"
    print "    p.x = 5"
    print "    println(\"overlay ${p.x}\")"
    print "    free(buf)"
    print "}"
}' > "$TMP/overlays.ae"
runs overlays "overlay 5"

awk 'BEGIN {
    print "import std.string"
    print "main() {"
    print "    s = string.concat(\"a\", \"b\")"
    print "    n = 0"
    for (i = 0; i < 300; i++) printf "    n = n + %d\n", i
    print "    v = string.bytes(s)"
    print "    free(v)"
    print "}"
}' > "$TMP/late_binding.ae"
refused late_binding "cannot free 'v': it is a read-only view of a string's bytes"

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] $NAME: names, types, arms, members, keys, defers, moves, parameters and places past every old cap"
exit 0
