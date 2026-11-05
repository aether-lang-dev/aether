#!/bin/sh
# #3545: the module merge has no fixed limit that drops or truncates what it
# reads past it. Each list grows with what it lists and each name is as long
# as it is written; a limit that is real is a diagnostic naming it.
#
# Each case passed a limit the merge used to stop at without a word:
#   locals:   a merged function's locals were read into 128 slots, so a local
#             past the 128th named like a module function was taken for the
#             function (its read renamed to `<ns>_<name>`).
#   aliases:  a module's aliased imports (`import m as a`) were read into 64
#             slots, so `a.f()` past the 64th kept a namespace nothing binds.
#   wide:     the modules reached through the imports of the program's direct
#             imports were read into 256 slots. A module past them is merged
#             all the same, through the imports the merge copies into the
#             program (#870), so this case holds that the 300th stays merged
#             with the list now as long as it needs.
#   long:     names were joined into 256-byte buffers, so two functions whose
#             names differ past that merged under one C name, and a call of
#             either inside their module reached that one.
#   shadow:   a selective import's names were read into 256 slots for the
#             check that a local function does not shadow one, so a shadowing
#             function past the 256th was not refused (E1000).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=module_merge_no_caps

[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fails=0

# runs <case> <want>: `ae run` of $TMP/<case>/main.ae prints <want>.
runs() {
    out=$(cd "$TMP/$1" && AETHER_HOME="$ROOT" "$AE" run main.ae 2>&1)
    case "$out" in
        *"$2"*) ;;
        *) echo "  [FAIL] $NAME: $1: not \"$2\":"
           echo "$out" | head -10 | sed 's/^/        /'
           fails=$((fails + 1)) ;;
    esac
}

# locals: 140 locals, the 135th named like the module's function.
mkdir -p "$TMP/locals/lib/loc"
awk 'BEGIN {
    print "helper() -> int {"
    print "    return 1"
    print "}"
    print ""
    print "many() -> int {"
    for (i = 0; i < 134; i++) print "    _l" i " = " i
    print "    helper = 5"
    for (i = 134; i < 140; i++) print "    _l" i " = " i
    print "    return helper + _l0"
    print "}"
}' > "$TMP/locals/lib/loc/module.ae"
printf 'import loc\n\nmain() {\n    println("many ${loc.many()} helper ${loc.helper()}")\n}\n' \
    > "$TMP/locals/main.ae"
runs locals "many 5 helper 1"

# The 300 modules the next cases import: m<i>.v() is i.
mkdir -p "$TMP/mods"
i=0
while [ $i -lt 300 ]; do
    mkdir -p "$TMP/mods/m$i"
    printf 'v() -> int {\n    return %d\n}\n' $i > "$TMP/mods/m$i/module.ae"
    i=$((i + 1))
done

# aliases: a module importing 70 modules each under an alias of its own.
mkdir -p "$TMP/aliases/lib/hub"
cp -r "$TMP/mods/"* "$TMP/aliases/lib/"
awk 'BEGIN {
    for (i = 0; i < 70; i++) print "import m" i " as a" i
    print ""
    print "total() -> int {"
    print "    return a0.v() + a69.v()"
    print "}"
}' > "$TMP/aliases/lib/hub/module.ae"
printf 'import hub\n\nmain() {\n    println("aliases ${hub.total()}")\n}\n' > "$TMP/aliases/main.ae"
runs aliases "aliases 69"

# wide: the program's one import imports 300 modules.
mkdir -p "$TMP/wide/lib/hub"
cp -r "$TMP/mods/"* "$TMP/wide/lib/"
awk 'BEGIN {
    for (i = 0; i < 300; i++) print "import m" i
    print ""
    print "total() -> int {"
    print "    return m0.v() + m299.v()"
    print "}"
}' > "$TMP/wide/lib/hub/module.ae"
printf 'import hub\n\nmain() {\n    println("wide ${hub.total()}")\n}\n' > "$TMP/wide/main.ae"
runs wide "wide 299"

# long: two functions whose 300-character names differ in the last one,
# called inside their module, which the merge renames.
mkdir -p "$TMP/long/lib/ln"
stem=$(awk 'BEGIN { s = "f"; for (i = 0; i < 300; i++) s = s "x"; print s }')
printf '%sa() -> int {\n    return 1\n}\n\n%sb() -> int {\n    return 2\n}\n\ntotal() -> int {\n    return %sa() * 10 + %sb()\n}\n' \
    "$stem" "$stem" "$stem" "$stem" > "$TMP/long/lib/ln/module.ae"
printf 'import ln\n\nmain() {\n    println("long ${ln.total()}")\n}\n' > "$TMP/long/main.ae"
runs long "long 12"

# shadow: 300 selected names, the program defining the last one itself.
mkdir -p "$TMP/shadow/lib/sel"
awk 'BEGIN {
    for (i = 0; i < 300; i++) { print "n" i "() -> int {"; print "    return " i; print "}"; print "" }
}' > "$TMP/shadow/lib/sel/module.ae"
awk 'BEGIN {
    s = "import sel ("
    for (i = 0; i < 300; i++) s = s (i ? ", " : "") "n" i
    print s ")"
    print ""
    print "n299() -> int {"
    print "    return -1"
    print "}"
    print ""
    print "main() {"
    print "    println(\"shadow ${n299()}\")"
    print "}"
}' > "$TMP/shadow/main.ae"
out=$(cd "$TMP/shadow" && AETHER_HOME="$ROOT" "$AE" run main.ae 2>&1)
case "$out" in
    *"error[E1000]"*"defines local function 'n299'"*) ;;
    *) echo "  [FAIL] $NAME: shadow: the local n299 past 256 selected names was not refused (E1000):"
       echo "$out" | head -10 | sed 's/^/        /'
       fails=$((fails + 1)) ;;
esac

if [ "$fails" -ne 0 ]; then
    exit 1
fi
echo "  [PASS] $NAME: locals past 128, 70 aliased imports, 300 modules, 300-character names and 300 selected names merge whole"
exit 0
