#!/bin/sh
# A closure may reassign its own parameter (#2462).
#
# The closure body was emitted with its parameters missing from the set of
# declared names, so the first assignment to one became a declaration: the
# C compiler stopped at "'n' redeclared as different kind of symbol", and a
# string parameter was also re-declared as `const char* s = NULL;` by the
# heap-string hoist. The parameters are now declared by the closure's
# signature, the way a function's are.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] closure_param_reassign: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
main() {
    g = |n: int| {
        n = n + 1
        println("n ${n}")
    }
    call(g, 10)

    k = |n: int| {
        n += 5
        n = n * 2
        println("k ${n}")
    }
    call(k, 1)

    // A parameter that shadows an outer name stays the closure's own.
    s = "outer"
    h = |s: string| {
        s = "${s}!"
        println(s)
    }
    call(h, "in")
    println(s)

    // Reassigned in a loop inside the closure.
    count_down = |left: int| {
        while left > 0 {
            left = left - 1
        }
        println("left ${left}")
    }
    call(count_down, 3)
}
AE

want="n 11
k 12
in!
outer
left 0"
got="$("$AE" run "$tmp/main.ae" 2>&1 | tr -d '\r' | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] closure_param_reassign: wrong output"
    printf 'got:\n%s\nwant:\n%s\n' "$got" "$want" | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] closure_param_reassign: a closure reassigns its int and string parameters"
