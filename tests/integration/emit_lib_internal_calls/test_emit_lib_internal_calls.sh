#!/bin/sh
# A library's calls to its own functions stay inside it (#2937).
#
# An --emit=lib library exported each function under its plain name as well
# as its aether_<name> alias, and on ELF a call through an exported name binds
# to the first definition in the process. host.c, linked against the library,
# defines its own add1; the library's twice must still call the library's.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] emit_lib_internal_calls: ae not built"; exit 0; }
CC="${CC:-cc}"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) SO_EXT=".dll"; EXE=".exe" ;;
    Darwin) SO_EXT=".dylib"; EXE="" ;;
    *) SO_EXT=".so"; EXE="" ;;
esac

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT
fail() { echo "  [FAIL] emit_lib_internal_calls: $1"; [ -n "$2" ] && [ -f "$2" ] && sed 's/^/    /' "$2" | head -20; exit 1; }

cp "$SCRIPT_DIR/calc.ae" "$TMPDIR/"
cd "$TMPDIR"
AETHER_HOME="$ROOT" "$AE" build --emit=lib calc.ae -o "libcalc$SO_EXT" >build.log 2>&1 || fail "ae build --emit=lib" build.log
$CC "$SCRIPT_DIR/host.c" "./libcalc$SO_EXT" -Wl,-rpath,"$TMPDIR" -o "host$EXE" >cc.log 2>&1 || fail "cc host.c" cc.log
OUT="$("./host$EXE" 2>&1)" || { echo "$OUT"; fail "the library called the host's function"; }
echo "$OUT" | grep -q "^OK 3 100$" || { echo "$OUT"; fail "unexpected output"; }
echo "  [PASS] emit_lib_internal_calls"
