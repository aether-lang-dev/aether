#!/bin/sh
# `call()` on a value that is not a closure is a type error (#2468).
#
# `call(x, ...)` lowers to `x.fn(x.env, ...)`. A callee whose type was known
# and was not a closure passed the type checker, and the only report was the
# C compiler's "'_tuple_ptr_string' has no member named 'fn'" against
# generated code. The usual way in is a `(value, err)` return such as
# `list.get` bound to one name. The checker now reports it at the argument
# with the fix, and every closure-valued callee still compiles and runs.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] call_non_closure_reject: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"
cd "$tmp" || exit 1

fail() {
    echo "  [FAIL] call_non_closure_reject: $1"
    [ -n "$2" ] && [ -f "$2" ] && sed 's/^/        /' "$2" | head -20
    exit 1
}

# expect_reject <name> <want-message> <want-help> : main.ae must fail type
# checking with that message at line 5, column 10, and never reach C.
expect_reject() {
    if "$AE" check main.ae > "$1.log" 2>&1; then
        fail "$1: call() on a non-closure was accepted" "$1.log"
    fi
    tr -d '\r' < "$1.log" > "$1.txt"
    grep -qF "$2" "$1.txt" || fail "$1: missing '$2'" "$1.txt"
    grep -qF -- "--> main.ae:5:10" "$1.txt" || fail "$1: not reported at the argument" "$1.txt"
    grep -qF "$3" "$1.txt" || fail "$1: missing help '$3'" "$1.txt"
    if grep -q "has no member named\|not a structure or union" "$1.txt"; then
        fail "$1: still reported by the C compiler" "$1.txt"
    fi
}

# The issue's program: list.get returns (ptr, string).
cat > main.ae <<'AE'
import std.list
main() {
    fs = list.list_new()
    cl = list.get(fs, 0)
    call(cl, 1)
}
AE
expect_reject tuple "call() needs a closure, but 'cl' has type (ptr, string)" \
    "destructure it (\`value, err = ...\`)"

cat > main.ae <<'AE'
import std.list
main() {
    fs = list.list_new()
    cl = list.get_raw(fs, 0)
    call(cl, 1)
}
AE
expect_reject ptr "call() needs a closure, but 'cl' has type ptr" \
    "call(unbox_closure(p), ...)"

cat > main.ae <<'AE'
struct P { a: int }
main() {
    n = 3
    p = P { a: n }
    call(n, p.a)
}
AE
expect_reject int "call() needs a closure, but 'n' has type int" \
    "pass a closure, a \`fn\`-typed value, or a function name"

# Every closure-valued callee still compiles and runs.
cat > main.ae <<'AE'
import std.list

struct S { cb: fn }

add1(n: int) -> int { return n + 1 }

mk(k: int) -> fn { return |n: int| { return n * k } }

apply(f: fn, x: int) -> int { return call(f, x) }

main() {
    s = S { cb: |n: int| { return n + 10 } }
    a = call(add1, 1)
    int b = call(mk(3), 2)
    int c = call(s.cb, 1)
    d = apply(add1, 5)
    int e = call(|n: int| { return n - 1 }, 9)
    hs = list.new()
    list.add(hs, box_closure(|n: int| { println("boxed ${n}") }))
    call(unbox_closure(list.get_raw(hs, 0)), 4)
    list.free(hs)
    println("${a} ${b} ${c} ${d} ${e}")
}
AE
want="boxed 4
2 6 11 6 8"
got="$("$AE" run main.ae 2>&1 | tr -d '\r' | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] call_non_closure_reject: a closure callee stopped working"
    printf 'got:\n%s\nwant:\n%s\n' "$got" "$want" | sed 's/^/        /'
    exit 1
fi

echo "  [PASS] call_non_closure_reject: call() on a tuple, ptr or int is a type error; closures still call"
