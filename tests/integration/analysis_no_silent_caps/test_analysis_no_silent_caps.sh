#!/bin/sh
# #3574: the analysis passes follow everything the program has. Each case
# passed a fixed array's size, and past it the pass checked nothing, or
# checked the wrong thing, without a word:
#
#   free_vars:     130 locals, the last freed twice: the free check followed
#                  128 and let the second free through.
#   frees_params:  17 `@frees` parameters, the last never freed: 16 were
#                  checked.
#   protocol_vars: 65 references with a protocol, the last sent a message its
#                  state does not take: the protocol check followed 64.
#   switch_arms:   a switch of 300 cases where only the last two bind the
#                  name read after it: the hoists read 256 arms and declared
#                  it nowhere.
#   nested_match:  a loop whose body binds a name 70 matches deep, read after
#                  the loop: the loop hoist went 64 deep and declared it
#                  nowhere.
#   quote_cycle:   300 @quotable functions calling round: the recursion check
#                  followed 256 deep and found no cycle.
#   quote_name:    a @quotable function of a 300-character name, called
#                  qualified in a quote: its name was cut to 256 bytes, and
#                  the quote carried no tree of it.
#   deep_call:     a closure that binds a view's name again, called 300 deep
#                  in an expression: the escape check's path held 256 nodes,
#                  missed the call, and refused the return of the name.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=analysis_no_silent_caps

[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fails=0

# runs <dir> <want>: `ae run` of $TMP/<dir>/main.ae prints <want>.
runs() {
    out=$(cd "$TMP/$1" && AETHER_HOME="$ROOT" "$AE" run main.ae 2>&1 | tr -d '\r')
    if [ "$out" != "$2" ]; then
        echo "  [FAIL] $NAME: $1: not \"$2\":"
        printf '%s\n' "$out" | cut -c1-160 | head -8 | sed 's/^/        /'
        fails=$((fails + 1))
    fi
}

# refused <dir> <text>: `ae check` of $TMP/<dir>/main.ae fails, saying <text>.
refused() {
    out=$(cd "$TMP/$1" && AETHER_HOME="$ROOT" "$AE" check main.ae 2>&1)
    rc=$?
    if [ $rc -eq 0 ] || ! printf '%s' "$out" | grep -qF "$2"; then
        echo "  [FAIL] $NAME: $1: not refused with \"$2\":"
        printf '%s\n' "$out" | cut -c1-160 | head -8 | sed 's/^/        /'
        fails=$((fails + 1))
    fi
}

# accepted <dir>: `ae check` of $TMP/<dir>/main.ae passes.
accepted() {
    out=$(cd "$TMP/$1" && AETHER_HOME="$ROOT" "$AE" check main.ae 2>&1)
    if [ $? -ne 0 ]; then
        echo "  [FAIL] $NAME: $1: refused:"
        printf '%s\n' "$out" | cut -c1-160 | head -8 | sed 's/^/        /'
        fails=$((fails + 1))
    fi
}

mkdir -p "$TMP/free_vars"
awk 'BEGIN {
    print "import std.bytes"
    print "main() {"
    for (i = 0; i < 130; i++) printf "    b%d = bytes.new(1)\n", i
    for (i = 0; i < 129; i++) printf "    bytes.free(b%d)\n", i
    print "    bytes.free(b129)"
    print "    bytes.free(b129)"
    print "}"
}' > "$TMP/free_vars/main.ae"
refused free_vars "'b129' is freed again"

mkdir -p "$TMP/frees_params"
awk 'BEGIN {
    print "import std.bytes"
    printf "drop("
    for (i = 0; i < 17; i++) printf "%sp%d: @frees ptr", (i ? ", " : ""), i
    print ") {"
    for (i = 0; i < 16; i++) printf "    bytes.free(p%d)\n", i
    print "}"
    print "main() { println(\"x\") }"
}' > "$TMP/frees_params/main.ae"
refused frees_params "'p16' is declared \`@frees\`, but a path reaches here without freeing it"

mkdir -p "$TMP/protocol_vars"
awk 'BEGIN {
    print "message Open {}"
    print "message Write {}"
    print "protocol W {"
    print "    start -> Open -> open"
    print "    open -> Write -> open"
    print "}"
    print "actor A implements W {"
    print "    receive {"
    print "        Open() -> {}"
    print "        Write() -> {}"
    print "    }"
    print "}"
    print "main() {"
    for (i = 0; i < 65; i++) printf "    w%d = spawn(A())\n", i
    for (i = 0; i < 64; i++) printf "    w%d ! Open {}\n", i
    print "    w64 ! Write {}"
    print "    wait_for_idle()"
    print "}"
}' > "$TMP/protocol_vars/main.ae"
refused protocol_vars "protocol 'W' does not allow 'Write' here: 'w64' is in state 'start'"

mkdir -p "$TMP/switch_arms"
awk 'BEGIN {
    print "pick(n: int) -> int {"
    print "    switch n {"
    for (i = 0; i < 299; i++) printf "        case %d: println(\"%d\")\n", i, i
    print "        case 299: y = 7"
    print "        default: y = -1"
    print "    }"
    print "    return y"
    print "}"
    print "main() { println(\"switch ${pick(299)}\") }"
}' > "$TMP/switch_arms/main.ae"
runs switch_arms "switch 7"

mkdir -p "$TMP/nested_match"
awk 'BEGIN {
    print "main() {"
    print "    total = 0"
    print "    i = 0"
    print "    while i < 2 {"
    for (d = 0; d < 70; d++) printf "        match %d { _ -> {\n", d
    print "        v = i + 1"
    print "        total = total + v"
    for (d = 0; d < 70; d++) print "        } }"
    print "        i = i + 1"
    print "    }"
    print "    println(\"nested ${total} ${v}\")"
    print "}"
}' > "$TMP/nested_match/main.ae"
runs nested_match "nested 3 2"

mkdir -p "$TMP/quote_cycle"
awk 'BEGIN {
    print "import std.quote"
    print "take(t: quote.Tree) {}"
    for (i = 0; i < 300; i++) {
        print "@quotable"
        printf "h%d(x: float) -> float { return h%d(x) }\n", i, (i + 1) % 300
    }
    print "main() {"
    print "    take() quote | r: float | { return h0(r) }"
    print "}"
}' > "$TMP/quote_cycle/main.ae"
refused quote_cycle "@quotable function 'h0' reaches itself"

mkdir -p "$TMP/quote_name/lib/qm"
LONGQ=$(awk 'BEGIN { s = "q"; while (length(s) < 300) s = s "x"; print s }')
cat > "$TMP/quote_name/lib/qm/module.ae" <<AE
@quotable
$LONGQ(x: float) -> float { return x * 2.0 }
AE
cat > "$TMP/quote_name/main.ae" <<AE
import std.quote
import qm

q(t: quote.Tree) -> quote.Tree { return t }

main() {
    t = q() quote | r: float | { return qm.$LONGQ(r) }
    println("helpers \${quote.helper_count(t)}")
    quote.free(t)
}
AE
runs quote_name "helpers 1"

mkdir -p "$TMP/deep_call"
awk 'BEGIN {
    print "loc() -> int[] {"
    print "    let a: int[3] = [4, 5, 6]"
    print "    s = a[0..2]"
    print "    reset = || {"
    print "        s = make([]int, 1)"
    print "        return 0"
    print "    }"
    printf "    _n = reset()"
    for (i = 0; i < 300; i++) printf " + 0"
    print ""
    print "    return s"
    print "}"
    print "main() { println(\"deep ${loc().len}\") }"
}' > "$TMP/deep_call/main.ae"
runs deep_call "deep 1"

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] $NAME: locals, @frees parameters, protocol references, arms, matches, helpers and paths past every old cap"
exit 0
