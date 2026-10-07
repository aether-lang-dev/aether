#!/bin/sh
# Postfix `i++` / `i--` yields the value from before the step (#2457).
#
# The parser built the same node for `i++` as for `++i`, so codegen emitted
# the prefix form wherever the value was used: `j = i++` gave the new value,
# `xs[k++]` skipped the first element, and `while n-- > 0` ran one iteration
# short. No diagnostic, just wrong numbers. The node now carries a postfix
# marker and reaches C as postfix. A `++` / `--` written as a statement of
# its own reads the same either way and is checked here too, as are a
# struct field, an element, a field through a pointer, a closure capture
# (which `x++` must promote to a shared cell, as `x += 1` does) and actor
# state.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] postfix_increment_value: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
struct P { x: int }

message Inc {}
message Get {}

actor C {
    state count = 0
    receive {
        Inc() -> {
            old = count++
            println("old ${old}")
        }
        Get() -> { reply count }
    }
}

make_counter() -> fn {
    c = 0
    return || {
        before = c++
        return before
    }
}

bump(p: *P) -> int {
    return p.x++
}

main() {
    i = 5
    j = i++
    println("i ${i} j ${j}")
    m = 5
    q = m--
    r = ++m
    println("m ${m} q ${q} r ${r}")

    int[3] xs = [10, 20, 30]
    k = 0
    first = xs[k++]
    second = xs[k++]
    println("first ${first} second ${second} k ${k}")

    n = 3
    loops = 0
    while n-- > 0 { loops += 1 }
    println("loops ${loops} n ${n}")

    p = P { x: 1 }
    px = p.x++
    println("p.x ${p.x} px ${px}")
    e = xs[1]++
    println("e ${e} xs1 ${xs[1]}")

    hp = heap.new(P)
    hp.x = 7
    b = bump(hp)
    println("b ${b} hp.x ${hp.x}")
    heap.free(hp)

    ctr = make_counter()
    int v0 = call(ctr)
    int v1 = call(ctr)
    println("v0 ${v0} v1 ${v1}")

    z = 1
    z++
    z--
    z--
    println("z ${z}")
    neg = -i++
    println("neg ${neg} i ${i}")

    a = spawn(C())
    a ! Inc {}
    a ! Inc {}
    got = a ? Get {}
    println("count ${got}")
}
AE

want="i 6 j 5
m 5 q 5 r 5
first 10 second 20 k 2
loops 3 n -1
p.x 2 px 1
e 20 xs1 21
b 7 hp.x 8
v0 0 v1 1
z 0
neg -6 i 7
old 0
old 1
count 2"
got="$("$AE" run "$tmp/main.ae" 2>&1 | tr -d '\r' | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] postfix_increment_value: wrong values"
    printf 'got:\n%s\nwant:\n%s\n' "$got" "$want" | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] postfix_increment_value: i++ / i-- yield the old value on locals, elements, fields, captures and actor state"
