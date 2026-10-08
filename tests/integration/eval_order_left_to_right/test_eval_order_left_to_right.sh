#!/bin/sh
# Operands are evaluated left to right (#2478).
#
# An interpolation, a call's arguments, the two sides of `+`, a struct
# literal, a message, an array literal and a returned tuple all lowered to
# a C construct that leaves the order of its operands unspecified, and two
# unsequenced writes of one variable (`f(i++, i++)`) are undefined
# behaviour. GCC evaluates call arguments right to left on Windows, so
# `"${j++} ${j++} ${j}"` printed `1 0 2` and `add(next(), counter)` read the
# global before the call that bumps it. An operand that conflicts with a
# later one (one writes a variable the other uses, or a call changes what
# the other reads) is now evaluated first, into a temporary, in source
# order. An array literal bound to an array that exists is evaluated whole
# before it is stored, so `a = [a[2], a[1], a[0]]` reverses it.
#
# The second program checks that a list with no such pair is emitted as
# before, without temporaries.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] eval_order_left_to_right: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
import std.string

struct P { a: int, b: int, c: int }
struct Box { n: int }

message Pair { a: int, b: int }
message Get {}

actor Rec {
    state int sum
    receive {
        Pair(a, b) -> { sum = sum * 100 + a * 10 + b }
        Get() -> { reply sum }
    }
}

var counter = 0

next() -> int {
    counter = counter + 1
    return counter
}

show3(a: int, b: int, c: int) -> string {
    return "${a} ${b} ${c}"
}

add(a: int, b: int) -> int { return a * 10 + b }

two(n: int) -> (int, int) {
    i = n
    return i++, i
}

take(b: *Box) -> int {
    b.n = b.n + 1
    return b.n
}

main() {
    j = 0
    println("interp ${j++} ${j++} ${j}")
    r = 2
    println("dec ${r--} ${--r} ${r}")
    i = 0
    println("call ${show3(i++, i++, i)}")
    s = 0
    println("assign ${add(s = s + 1, s)} ${s}")
    k = 1
    println("module ${string.substring("abcdef", k++, k + 2)}")
    m = 1
    p = P { a: m++, b: m++, c: m }
    println("struct ${p.a} ${p.b} ${p.c}")
    n = 1
    arr = [n++, n++, n]
    println("array ${arr[0]} ${arr[1]} ${arr[2]}")
    arr = [arr[2], arr[1], arr[0]]
    println("rebind ${arr[0]} ${arr[1]} ${arr[2]}")
    x = 5
    println("binary ${x++ + x++} ${x}")
    q = 3
    println("nested ${add(add(q++, q++), q++)} ${q}")
    t1, t2 = two(5)
    println("tuple ${t1} ${t2}")
    println("global ${add(next(), counter)} ${add(next(), next())}")
    c = 0
    f = || {
        c = c + 5
        return c
    }
    println("closure ${add(call(f), c)}")
    bx = heap.new(Box)
    bx.n = 1
    println("pointer ${add(take(bx), bx.n)}")
    heap.free(bx)
    rec = spawn(Rec())
    v = 1
    rec ! Pair { a: v++, b: v }
    total = rec ? Get {}
    println("message ${total} ${v}")
}
AE

want="interp 0 1 2
dec 2 0 0
call 0 1 2
assign 11 1
module bcd
struct 1 2 3
array 1 2 3
rebind 3 2 1
binary 11 7
nested 345 6
tuple 5 6
global 11 23
closure 55
pointer 22
message 12 2"
got="$("$AE" run "$tmp/main.ae" 2>&1 | tr -d '\r' | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] eval_order_left_to_right: operands not evaluated left to right"
    printf 'got:\n%s\nwant:\n%s\n' "$got" "$want" | sed 's/^/        /'
    exit 1
fi

# No operand here writes what another reads: `sq` writes nothing, and the
# reads are of plain locals no call can reach. The C stays as it was.
cat > "$tmp/plain.ae" <<'AE'
sq(n: int) -> int { return n * n }

pick(a: int, b: int, c: int) -> int { return a + b + c }

main() {
    a = 3
    b = 4
    z = sq(a) + sq(b)
    w = pick(sq(a), b, sq(sq(b)))
    k = a++
    println(z + w + k)
}
AE
if ! "$AETHERC" "$tmp/plain.ae" "$tmp/plain.c" >/dev/null 2>&1; then
    echo "  [FAIL] eval_order_left_to_right: plain.ae did not compile"
    exit 1
fi
if grep -q "_eo[0-9]" "$tmp/plain.c"; then
    echo "  [FAIL] eval_order_left_to_right: operands with nothing to order were hoisted"
    grep -n "_eo[0-9]" "$tmp/plain.c" | head -5 | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] eval_order_left_to_right: operands are evaluated left to right, and only hoisted when another operand depends on it"
