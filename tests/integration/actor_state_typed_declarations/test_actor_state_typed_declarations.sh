#!/bin/sh
# `state <type> <name>` takes every scalar type the language reference lists,
# and `ptr p = null` declares a local (#2465).
#
# The actor-state parser recognised a typed declaration only by a fixed list
# of keyword tokens. `uint8`, `uint16`, `uint32`, `int64`, `f32` and
# `longdouble` lex as identifiers, so `state uint8 b8 = 250` declared a field
# named `uint8` and left `b8 = 250` behind as a stray statement: "'b8'
# undeclared" in the C compiler. `ptr` was missing from the list as well,
# and a declaration starting with it was dropped. The state parser now takes
# the same `TYPE NAME` shape as a typed local. Separately, `ptr p = null` at
# statement start was parsed as an expression on a variable named `ptr`
# ("Undefined variable 'ptr'"); `ptr NAME` on one line now declares.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] actor_state_typed_declarations: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
message Bump { b: int }
message Get8 {}
message Get16 {}
message Get64 {}
message GetP {}

actor A {
    state uint8 b8 = 250
    state uint16 h16 = 65535
    state uint32 w32 = 4000000000
    state int64 big = 5
    state f32 f = 1.5
    state ptr p = null
    receive {
        Bump(b) -> {
            b8 += b
            h16 += b
            w32 += b
            big = big * b
            f = f * 2.0
        }
        Get8() -> { reply b8 }
        Get16() -> { reply h16 }
        Get64() -> { reply big }
        GetP() -> {
            if p == null { reply 1 } else { reply 0 }
        }
    }
}

main() {
    a = spawn(A())
    a ! Bump { b: 10 }
    v8 = a ? Get8 {}
    v16 = a ? Get16 {}
    v64 = a ? Get64 {}
    vp = a ? GetP {}
    println("b8 ${v8} h16 ${v16}")
    println("big ${v64} null ${vp}")

    ptr q = null
    if q == null { println("q null") }
}
AE

want="b8 4 h16 9
big 50 null 1
q null"
got="$("$AE" run "$tmp/main.ae" 2>&1 | tr -d '\r' | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] actor_state_typed_declarations: wrong output"
    printf 'got:\n%s\nwant:\n%s\n' "$got" "$want" | sed 's/^/        /'
    exit 1
fi
# The declared widths reach the C struct.
if ! "$ROOT/build/aetherc" "$tmp/main.ae" "$tmp/out.c" >/dev/null 2>&1; then
    echo "  [FAIL] actor_state_typed_declarations: aetherc failed"
    exit 1
fi
for decl in 'uint8_t b8;' 'uint16_t h16;' 'uint32_t w32;' 'float f;' 'void\* p;'; do
    if ! grep -q "$decl" "$tmp/out.c"; then
        echo "  [FAIL] actor_state_typed_declarations: no '$decl' field in the actor struct"
        exit 1
    fi
done
echo "  [PASS] actor_state_typed_declarations: state uint8/uint16/uint32/int64/f32/ptr fields, and a ptr local"
