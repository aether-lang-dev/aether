#!/bin/sh
# A `defer` in a `match` arm block runs when that arm ends, and only when
# that arm was taken (#2459).
#
# A match arm's block was emitted without a defer scope of its own, so its
# defers joined the enclosing scope's: they ran at the end of the function
# (or loop body) whichever arm had been taken, and in a loop on every
# iteration. An `if` / `switch` arm block is already its own scope; a match
# arm is now one the same way, so a defer runs at the arm's end, on a
# `break` out of it and before a `return` from it. The same scope stops a
# name declared in one arm leaking into the next, where it was emitted as an
# assignment to an undeclared C variable.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] match_arm_defer_scope: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
f(x: int) {
    match x {
        1 -> {
            defer println("cleanup-1")
            y = 10
            println("one ${y}")
        }
        2 -> {
            y = 20
            println("two ${y}")
        }
        _ -> { println("other ${x}") }
    }
    println("after ${x}")
}

loop(stop: int) {
    for i in 0..3 {
        match i {
            1 -> {
                defer println("arm-defer ${i}")
                if stop == 1 { break }
                println("arm ${i}")
            }
            _ -> { println("i ${i}") }
        }
    }
}

early(x: int) -> int {
    match x {
        1 -> {
            defer println("ret-defer")
            return 1
        }
        _ -> { println("early other") }
    }
    return 0
}

main() {
    f(2)
    f(1)
    f(3)
    loop(0)
    loop(1)
    println("early ${early(1)}")
    println("early ${early(2)}")
}
AE

want="two 20
after 2
one 10
cleanup-1
after 1
other 3
after 3
i 0
arm 1
arm-defer 1
i 2
i 0
arm-defer 1
ret-defer
early 1
early other
early 0"
got="$("$AE" run "$tmp/main.ae" 2>&1 | tr -d '\r' | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] match_arm_defer_scope: wrong output"
    printf 'got:\n%s\nwant:\n%s\n' "$got" "$want" | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] match_arm_defer_scope: a match arm's defer runs at the arm's end, only for the arm taken"
