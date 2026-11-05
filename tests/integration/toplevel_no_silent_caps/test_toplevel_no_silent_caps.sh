#!/bin/sh
# #3574: the compiler's driver keeps every name and list it is given. Each
# case passed a fixed array's size, and past it aetherc dropped or cut what
# it was given:
#
#   stub_dirs:     nine --binimport-stub-dir directories, the module of the
#                  last one declaring an extern: eight were kept, and the
#                  ninth's extern was refused under --emit=lib as the
#                  program's own.
#   long_define:   a -D symbol of 200 characters: refused as "too many -D
#                  symbols" (127 bytes was the most).
#   inspect_tuple: --emit=inspect of a function returning a tuple of types
#                  of 300-character names: the return type was cut at 256
#                  bytes (and the offset past the cut ran beyond them).
#
# The artifact codec's header values and the build-symbol log, which held
# 512 bytes and 128 names, are tested in tests/compiler/test_aea.c.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
[ -x "$AETHERC" ] || [ ! -x "$AETHERC.exe" ] || AETHERC="$AETHERC.exe"
NAME=toplevel_no_silent_caps
[ -x "$AETHERC" ] || { echo "  [SKIP] $NAME: aetherc not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
AETHER_HOME="$ROOT"
export AETHER_HOME
fails=0

fail() {
    echo "  [FAIL] $NAME: $1"
    [ -n "${2:-}" ] && printf '%s\n' "$2" | cut -c1-160 | head -6 | sed 's/^/        /'
    fails=$((fails + 1))
}

long() {  # long <prefix> <length>
    awk -v p="$1" -v n="$2" 'BEGIN { s = p; while (length(s) < n) s = s "x"; print s }'
}

# --- stub_dirs ----------------------------------------------------------------
mkdir -p "$TMP/stub_dirs/lib/qx"
cat > "$TMP/stub_dirs/lib/qx/module.ae" <<'AE'
extern abs(x: int) -> int

twice(x: int) -> int {
    return abs(x) * 2
}
AE
cat > "$TMP/stub_dirs/main.ae" <<'AE'
import qx

doubled(x: int) -> int {
    return qx.twice(x)
}
AE
out=$(cd "$TMP/stub_dirs" && "$AETHERC" --emit=lib --binimport-stub-dir=d1 --binimport-stub-dir=d2 \
    --binimport-stub-dir=d3 --binimport-stub-dir=d4 --binimport-stub-dir=d5 \
    --binimport-stub-dir=d6 --binimport-stub-dir=d7 --binimport-stub-dir=d8 \
    --binimport-stub-dir=lib/qx main.ae out.c 2>&1)
[ $? -eq 0 ] || fail "stub_dirs: the ninth stub directory's extern was refused:" "$out"

# --- long_define --------------------------------------------------------------
mkdir -p "$TMP/long_define"
SYM=$(long D 200)
cat > "$TMP/long_define/main.ae" <<AE
main() {
    when defined($SYM) {
        println("defined")
    }
    println("end")
}
AE
out=$(cd "$TMP/long_define" && "$AETHERC" -D "$SYM" main.ae out.c 2>&1)
if [ $? -ne 0 ]; then
    fail "long_define: a 200-character -D symbol was refused:" "$out"
elif ! grep -q '"defined"' "$TMP/long_define/out.c"; then
    fail "long_define: the region the symbol enables is not in the C"
fi

# --- inspect_tuple ------------------------------------------------------------
mkdir -p "$TMP/inspect_tuple"
S=$(long S 300)
cat > "$TMP/inspect_tuple/main.ae" <<AE
struct $S {
    n: int
}

wide() -> ($S, int, $S) {
    return $S { n: 1 }, 2, $S { n: 3 }
}

main() {
    a, b, c = wide()
    println("\${b} \${a.n} \${c.n}")
}
AE
out=$(cd "$TMP/inspect_tuple" && "$AETHERC" --emit=inspect main.ae out.txt 2>&1 | tr -d '\r')
printf '%s\n' "$out" | grep -qF "wide() -> ($S, int, $S)" ||
    fail "inspect_tuple: the return type is not written whole:" "$(printf '%s\n' "$out" | grep -F 'wide()')"

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] $NAME: stub directories, -D symbols and inspected tuples past every old cap"
exit 0
