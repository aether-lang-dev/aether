#!/bin/sh
# A C file that a module declares with @source and that the program also
# names with --extra is compiled once, not twice. Before, the two lists were
# concatenated as they came, so a program still listing a module's C file
# (what a module asked for before @source existed, #2125) failed at link
# with every symbol in the file defined twice. Spellings that name the same
# file (relative, with `..`, absolute) count as the same file.
set -eu

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
AE="$ROOT/build/ae"
if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] source_dedupe: build/ae not built"
    exit 0
fi
AETHER_HOME=""
export AETHER_HOME

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fail() {
    echo "  [FAIL] source_dedupe: $1"
    exit 1
}

mkdir -p "$WORK/mymod"
cat > "$WORK/mymod/module.ae" <<'AE'
@source("helper.c")
exports (twice)
extern helper_value() -> int
twice() -> int { return helper_value() * 2 }
AE
cat > "$WORK/mymod/helper.c" <<'C'
int helper_value(void) { return 21; }
C
cat > "$WORK/main.ae" <<'AE'
import mymod
main() { println("${mymod.twice()}") }
AE

cd "$WORK"
for spelling in mymod/helper.c ./mymod/../mymod/helper.c "$WORK/mymod/helper.c"; do
    rm -f prog prog.exe
    out="$("$AE" build main.ae --extra "$spelling" -o prog 2>&1)" \
        || { echo "$out" | sed 's/^/    /' | tail -8; fail "listing $spelling beside the module's @source did not build"; }
    got="$(./prog)"
    [ "$got" = "42" ] || fail "expected 42 with --extra $spelling, got '$got'"
done

echo "  [PASS] source_dedupe: a module's @source named again with --extra is compiled once"
