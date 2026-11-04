#!/bin/sh
# #3545: a module with more than 4096 functions or constants has every one
# of them renamed in its merged bodies.
#
# The merge read a module's function and constant names into fixed arrays
# of 4096 and stopped at the cap without a word, so a merged body's
# reference to a name past it stayed bare: a read of the 4097th `var` failed
# "Undefined variable", and a bare call of a function past it that the
# program also defines bound to the program's function instead of the
# module's own. The lists are now as long as the module needs.
#
# The module here declares 5000 functions and 5000 vars, and one function
# that reads the last of each and calls a function past the cap that the
# program defines too, under the same name. The program prints what the
# module's function computes from its own declarations.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=module_names_past_cap

[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/lib/big"
awk -v n=5000 'BEGIN {
    for (i = 0; i < n; i++) print "var mv" i " = " i
    for (i = 0; i < n; i++) {
        print ""
        print "fn" i "() -> int {"
        print "    return " i
        print "}"
    }
    print ""
    print "// Past the cap: a var, a function, and one the program defines too."
    print "last() -> int {"
    print "    return mv" (n - 1) " + fn" (n - 1) "() + fn4500()"
    print "}"
}' > "$TMP/lib/big/module.ae"
cat > "$TMP/main.ae" <<'AE'
import big

// The program's own function of a name the module declares past the cap:
// the module's body calls its own.
fn4500() -> int {
    return -1
}

main() {
    println("last ${big.last()} own ${fn4500()}")
}
AE

out=$(cd "$TMP" && AETHER_HOME="$ROOT" "$AE" run main.ae 2>&1)
case "$out" in
    *"last 14498 own -1"*)
        echo "  [PASS] $NAME: a module of 10000 declarations renames every one in its merged bodies" ;;
    *)
        echo "  [FAIL] $NAME: not \"last 14498 own -1\":"
        echo "$out" | head -12 | sed 's/^/        /'
        exit 1 ;;
esac
exit 0
