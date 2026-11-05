#!/bin/sh
# #3569: the passes around the checker build names and paths as long as
# they are, too. Each joined them into a fixed buffer and cut a longer one:
#   derive:   `@derive(eq)` named its helper `<Struct>_eq` cut at 250
#             bytes, so a longer struct's helper was named nothing a call
#             names.
#   heredoc:  the lexer read a heredoc's marker into 256 bytes and skipped
#             the rest of the line, so no closing line matched the cut
#             marker and the heredoc never ended.
#   contract: a contract folded an enum member by joining `<Enum>_<Member>`
#             into 256 bytes, so a longer enum's member was not folded and
#             a call that breaks a precondition at compile time compiled.
#   escape:   the slice escape check joined a field path into 256 bytes, so
#             two fields whose names share their first 250 bytes were one
#             path, and returning the one that holds a parameter's slice
#             was refused for the one that holds the function's own.
#   trust:    sandbox.enforce joined a trusted call's C name into 512 bytes,
#             so a module's C extern reached under its prefixed name was not
#             seen to be one, and was trusted.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=analysis_long_names

[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fails=0

# A name of 600 characters starting with `$1`.
long() {
    awk -v p="$1" 'BEGIN { s = p; while (length(s) < 600) s = s "x"; print s }'
}

# runs <case> <want>: `ae run` of $TMP/<case>/main.ae prints <want>.
runs() {
    out=$(cd "$TMP/$1" && AETHER_HOME="$ROOT" "$AE" run main.ae 2>&1 | tr -d '\r')
    if [ "$out" != "$2" ]; then
        echo "  [FAIL] $NAME: $1: not \"$2\":"
        printf '%s\n' "$out" | cut -c1-160 | head -10 | sed 's/^/        /'
        fails=$((fails + 1))
    fi
}

# refused <case> <text>: `ae check` of $TMP/<case>/main.ae fails saying <text>.
refused() {
    out=$(cd "$TMP/$1" && AETHER_HOME="$ROOT" "$AE" check main.ae 2>&1)
    rc=$?
    if [ $rc -eq 0 ] || ! printf '%s' "$out" | grep -qF "$2"; then
        echo "  [FAIL] $NAME: $1: not refused with \"$2\":"
        printf '%s\n' "$out" | cut -c1-160 | head -10 | sed 's/^/        /'
        fails=$((fails + 1))
    fi
}

S=$(long S)
mkdir -p "$TMP/derive"
cat > "$TMP/derive/main.ae" <<AE
@derive(eq)
struct $S {
    x: int
}

main() {
    a = $S { x: 1 }
    b = $S { x: 1 }
    c = $S { x: 2 }
    println("eq \${${S}_eq(a, b)} \${${S}_eq(a, c)}")
}
AE
runs derive "eq 1 0"

M=$(long M)
mkdir -p "$TMP/heredoc"
cat > "$TMP/heredoc/main.ae" <<AE
main() {
    s = <<$M
inside
$M
    println("heredoc \${s}")
}
AE
runs heredoc "heredoc inside"

E=$(long E)
mkdir -p "$TMP/contract"
cat > "$TMP/contract/main.ae" <<AE
enum $E { Off, Low }

f(n: int) -> int requires n > 0 { return n }

main() {
    x = f($E.Off)
    println("\${x}")
}
AE
refused contract "precondition violation at compile time: n > 0"

P=$(awk 'BEGIN { s = "f"; while (length(s) < 300) s = s "x"; print s }')
mkdir -p "$TMP/escape"
cat > "$TMP/escape/main.ae" <<AE
struct Bag {
    ${P}a: int[]
    ${P}b: int[]
}

pick(m: int[]) -> int[] {
    let a: int[3] = [7, 8, 9]
    bag = Bag { ${P}a: a, ${P}b: m }
    return bag.${P}b
}

main() {
    m = make([]int, 2)
    println("escape \${pick(m).len}")
}
AE
runs escape "escape 2"

# Past the 512 bytes the C name had, and short enough for the diagnostic's
# own text to keep its last words.
X=$(awk 'BEGIN { s = "x"; while (length(s) < 520) s = s "x"; print s }')
mkdir -p "$TMP/trust/lib/lng"
cat > "$TMP/trust/lib/lng/module.ae" <<AE
extern lng_$X() -> int
AE
cat > "$TMP/trust/main.ae" <<AE
import std.sandbox
import lng

main() {
    w = sandbox.new("w") {}
    sandbox.enforce(w, lng) callback {
        _ = lng.$X()
    }
    sandbox.free(w)
}
AE
refused trust "is a C extern"

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] $NAME: a derive, a heredoc marker, a contract's enum, a field path and a trusted extern of long names"
exit 0
