#!/bin/sh
# A module's call to an extern it declares stays that extern when the
# program defines a function of the same name. Two passes took it to the
# program's function: the checker found the program's symbol, because the
# module's `extern` counted as a declaration of its own and so let the
# program's name through (#3088's own-scope rule then never fired), and
# codegen, renaming the program's function out of the extern's way (#1366,
# `abs` -> `ae_abs`), renamed every call of that name, the module's too.
#
#   pkg.cabs  declares `extern abs` and calls it; the program defines its
#             own `abs`. The module ran the program's, silently.
#   md5       std.cryptography's `extern free` beside a program's two-
#             argument `free`: every `free(ctx)` in std failed to check
#             against the program's, and the error pointed into std.
#
# The program's own calls still reach its own function.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=module_extern_beside_program_function

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] $NAME: $AE not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export AETHER_HOME="$ROOT"
fails=0

# run_ok <case> <want>: main.ae runs and prints <want> (its lines joined by
# '|').
run_ok() {
    got=$(cd "$TMP" && "$AE" run main.ae 2>&1 | tr -d '\r' | grep -v '^[[:space:]]*$' | paste -sd'|' -)
    if [ "$got" != "$2" ]; then
        echo "  [FAIL] $NAME/$1: expected '$2', got '$got'"
        fails=$((fails + 1))
    fi
}

mkdir -p "$TMP/lib/pkg/cabs"
cat > "$TMP/lib/pkg/cabs/module.ae" <<'AE'
extern abs(x: int) -> int

exports(magnitude)

magnitude(x: int) -> int {
    return abs(x)
}
AE

# The module's extern beside the program's function of its name.
cat > "$TMP/main.ae" <<'AE'
import pkg.cabs

abs(x: int) -> int {
    return 99
}

main() {
    println("module ${cabs.magnitude(-5)}")
    println("program ${abs(-5)}")
}
AE
run_ok local_module_extern "module 5|program 99"

# std's own extern free beside the program's two-argument free.
cat > "$TMP/main.ae" <<'AE'
import std.cryptography.md5
import std.string

free(p: ptr, n: int) -> int {
    return n
}

main() {
    ctx, err = md5.new()
    if err != "" {
        println("new failed: ${err}")
        return
    }
    md5.update(ctx, string.bytes("abc"))
    println(md5.final_hex(ctx))
    println("program ${free(null, 7)}")
}
AE
run_ok std_extern_free "900150983cd24fb0d6963f7d28e17f72|program 7"

if [ "$fails" -ne 0 ]; then
    exit 1
fi
echo "  [PASS] $NAME: a module's extern stays its own beside the program's function"
exit 0
