#!/bin/sh
# Regression (#2762 follow-up): a tuple destructure that re-binds a local
# follows the rules a bare `name = value` re-bind does, slot by slot.
#
# Since #2762 a closure's `a, b = two()` writes the outer `a` and `b` a
# destructure bound before it, as `a = 7` does. The type checker checked a
# destructure's slots against nothing, so a slot of another type than the
# local it re-binds got as far as the C compiler ("assignment to 'const
# char *' from 'int'"), or a float slot was truncated into an int local
# that `a = 3.5` would have been refused for. That happened as well with
# an outer `a = ...` and with a destructure in the local's own scope. Each
# slot now keeps the local's type: a value of another kind is refused with
# the re-bind error, one an inferred int would truncate with the narrowing
# error, both at the slot, and a value C converts (an int into a float
# local) is stored as one.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] closure_destructure_rebind_reject: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"
cd "$tmp" || exit 1

fail() {
    echo "  [FAIL] closure_destructure_rebind_reject: $1"
    [ -n "$2" ] && [ -f "$2" ] && sed 's/^/        /' "$2" | head -20
    exit 1
}

# expect_reject <name> <want-message> <line:col> : main.ae must fail type
# checking with that message at that slot, and never reach C.
expect_reject() {
    if "$AE" check main.ae > "$1.log" 2>&1; then
        fail "$1: the re-binding destructure was accepted" "$1.log"
    fi
    tr -d '\r' < "$1.log" > "$1.txt"
    grep -qF "$2" "$1.txt" || fail "$1: missing '$2'" "$1.txt"
    grep -qF -- "--> main.ae:$3" "$1.txt" || fail "$1: not reported at the slot ($3)" "$1.txt"
    if grep -q -E "makes pointer from integer|incompatible types" "$1.txt"; then
        fail "$1: still reported by the C compiler" "$1.txt"
    fi
}

# A closure's destructure re-binding a string a destructure bound.
cat > main.ae <<'AE'
import std.string
two() -> (int, int) {
    return 7, 8
}
names(p: string) -> (string, int) {
    return string.concat(p, "!"), 1
}
main() {
    a, b = names(string.concat("bc", ""))
    bump = || {
        a, b = two()
    }
    bump()
    println("${a} ${b}")
}
AE
expect_reject string_slot "cannot re-bind 'a' as int: it was bound as string" "11:9"

# A float slot into an int a destructure bound: it would truncate.
cat > main.ae <<'AE'
three() -> (int, int) {
    return 1, 2
}
halves() -> (float, int) {
    return 3.5, 4
}
main() {
    a, b = three()
    bump = || {
        a, b = halves()
    }
    bump()
    println("${a * 2} ${b}")
}
AE
expect_reject float_slot "narrowing assignment to 'a'" "10:9"

# A closure's destructure re-binding a string bound with `a = ...`.
cat > main.ae <<'AE'
import std.string
two() -> (int, int) {
    return 7, 8
}
main() {
    a = string.concat("bc", "")
    b = 1
    bump = || {
        a, b = two()
    }
    bump()
    println("${a} ${b}")
}
AE
expect_reject plain_outer "cannot re-bind 'a' as int: it was bound as string" "9:9"

# A destructure in the local's own scope.
cat > main.ae <<'AE'
import std.string
two() -> (int, int) {
    return 7, 8
}
main() {
    a = string.concat("bc", "")
    a, b = two()
    println("${a} ${b}")
}
AE
expect_reject same_scope "cannot re-bind 'a' as int: it was bound as string" "7:5"

# A value C converts is stored as the local's type: an int slot into a
# float local, in a closure and in the local's own scope.
cat > main.ae <<'AE'
two() -> (int, int) {
    return 7, 8
}
main() {
    a = 1.5
    b = 0
    bump = || {
        a, b = two()
    }
    bump()
    c = 0.5
    c, d = two()
    println("${a} ${b} ${c + 0.25} ${d}")
}
AE
if ! "$AE" build main.ae -o ok > build.log 2>&1; then
    fail "converting slots: the build failed" build.log
fi
./ok > run.log 2>&1 || fail "converting slots: the program failed" run.log
tr -d '\r' < run.log | grep -qx "7 8 7.25 8" || fail "converting slots: wrong output" run.log

echo "  [PASS] closure_destructure_rebind_reject: destructure slots keep the local's type"
exit 0
