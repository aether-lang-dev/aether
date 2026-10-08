#!/bin/sh
# A closure in a receive arm can capture the names the arm's message pattern
# binds (#2492).
#
# Capture discovery asked whether the arm declares a name by looking only at
# the declarations in the arm's body. The pattern's bindings (`Ping(n)`,
# `Named(who: name)`) are declared by the handler from the message, not by a
# statement, so a closure reading one captured nothing and its body read an
# undeclared `n` in C. The type of a capture in an arm (a binding or a local)
# was also looked up through every function rather than the arm, so a string
# local captured from an arm was declared int. A closure that writes a
# binding now gets it as a shared cell seeded from the message; a string
# cell takes its own reference, so the message's string is released once.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] closure_captures_receive_binding: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
import std.string

message Ping { n: int }
message Named { who: string, times: int }
message Raw { p: ptr, tag: string }
message Bump { by: int, label: string }
message Deep { d: int }
message Done {}

actor A {
    state total = 0
    receive {
        // The issue's shape.
        Ping(n) -> {
            g = || { println("n=${n}") }
            g()
        }
        // Bindings under another name, read two closures deep.
        Named(who: name, times: k) -> {
            say = | sep: string | {
                inner = || { println("${name}${sep}${k}") }
                inner()
            }
            say(":")
        }
        // A ptr payload and a string.
        Raw(p, tag) -> {
            look = || {
                if p != null {
                    println("${tag} has payload")
                }
            }
            look()
        }
        // A closure writes the bindings; the arm reads them after, and a
        // string local of the arm is captured as a string.
        Bump(by, label) -> {
            inc = || {
                by = by + 10
                label = string.concat(label, "+")
            }
            inc()
            inc()
            local = string.concat(label, "!")
            show = || { println("${by} ${local}") }
            show()
            total = total + by
        }
        // Written two closures deep.
        Deep(d) -> {
            outer = || {
                inner = || { d = d * 2 }
                inner()
                inner()
            }
            outer()
            println("d=${d}")
        }
        Done() -> {
            println("total=${total}")
        }
    }
}

main() {
    a = spawn(A())
    a ! Ping { n: 3 }
    a ! Named { who: "bo", times: 2 }
    x = 5
    a ! Raw { p: &x, tag: "raw" }
    i = 0
    while i < 3 {
        a ! Bump { by: i, label: "${i}" }
        i = i + 1
    }
    a ! Deep { d: 3 }
    a ! Done {}
    wait_for_idle()
}
AE

want="n=3
bo:2
raw has payload
20 0++!
21 1++!
22 2++!
d=12
total=63"
got="$("$AE" run "$tmp/main.ae" 2>&1 | tr -d '\r')"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] closure_captures_receive_binding: wrong output"
    printf 'got:\n%s\nwant:\n%s\n' "$got" "$want" | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] closure_captures_receive_binding: closures in receive arms capture and write the pattern's int, string and ptr bindings"
