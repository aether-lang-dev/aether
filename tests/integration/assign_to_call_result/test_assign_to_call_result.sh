#!/bin/sh
# Assigning to a call's result is a type error (#2481).
#
# A call's value is a temporary. `copy(q).x = 0.0` and `copy(q) = v` passed
# the type checker and stopped the C compiler at "lvalue required", against a
# line of generated code; `grid().cells[0] = 9` (a by-value array in the
# result) even compiled, and the write was lost. `f(x) += v` stopped the
# parser with "Expected statement in block". Each is now reported at the
# assignment. A field or element reached through a pointer is real storage
# whichever call produced the pointer, so those assignments still compile
# and write where they should.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] assign_to_call_result: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"
cd "$tmp" || exit 1

fail() {
    echo "  [FAIL] assign_to_call_result: $1"
    [ -n "$2" ] && [ -f "$2" ] && sed 's/^/        /' "$2" | head -30
    exit 1
}

cat > bad.ae <<'AE'
struct P {
    x: float
    y: float
}

struct Grid {
    cells: int[3]
}

struct Outer {
    inner: P
}

copy(p: P) -> P { return p }

grid() -> Grid { return Grid { cells: [1, 2, 3] } }

outer(p: P) -> Outer { return Outer { inner: p } }

next_int(x: int) -> int { return x + 1 }

main() {
    q = P { x: 1.0, y: 2.0 }
    copy(q).x = 0.0
    copy(q) = P { x: 3.0, y: 4.0 }
    grid().cells[0] = 9
    outer(q).inner.x = 3.0
    next_int(1) = 4
    println("${q.x}")
}
AE
if "$AE" check bad.ae > bad.log 2>&1; then
    fail "an assignment to a call result was accepted" bad.log
fi
tr -d '\r' < bad.log > bad.txt
# expect <line:col> <text>
expect() {
    grep -qF -- "--> bad.ae:$1" bad.txt || fail "nothing reported at bad.ae:$1" bad.txt
    grep -qF "$2" bad.txt || fail "missing '$2'" bad.txt
}
expect 24:15 "cannot assign to a part of the result of 'copy(...)': a call's result is a temporary"
expect 25:13 "cannot assign to the result of 'copy(...)'"
expect 26:21 "cannot assign to a part of the result of 'grid(...)'"
expect 27:22 "cannot assign to a part of the result of 'outer(...)'"
expect 28:17 "cannot assign to the result of 'next_int(...)'"
if grep -q "lvalue required" bad.txt; then
    fail "still reported by the C compiler" bad.txt
fi

# The compound form stops at the operator with the same reason.
cat > op.ae <<'AE'
next_int(x: int) -> int { return x + 1 }

main() {
    next_int(1) += 4
}
AE
if "$AE" check op.ae > op.log 2>&1; then
    fail "a compound assignment to a call result was accepted" op.log
fi
tr -d '\r' < op.log > op.txt
grep -qF "cannot assign to the result of a call: it is a temporary" op.txt ||
    fail "compound assignment to a call result: wrong diagnostic" op.txt

# Through a pointer the target is real storage.
cat > good.ae <<'AE'
struct P {
    x: float
    y: float
}

struct Box {
    p: *P
    n: int
}

struct Grid {
    cells: int[3]
}

holder(p: *P) -> *P { return p }

box_of(p: *P) -> Box { return Box { p: p, n: 1 } }

grid() -> Grid { return Grid { cells: [1, 2, 3] } }

slice_of(g: *Grid) -> int[] { return g.cells }

main() {
    q = P { x: 1.0, y: 2.0 }
    holder(&q).x = 5.0
    box_of(&q).p.y = 7.0
    g = grid()
    slice_of(&g)[1] = 20
    println("${q.x} ${q.y} ${g.cells[1]}")
}
AE
got="$("$AE" run good.ae 2>&1 | tr -d '\r' | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
if [ "$got" != "5 7 20" ]; then
    echo "  [FAIL] assign_to_call_result: an assignment through a returned pointer stopped working"
    printf 'got:\n%s\nwant:\n5 7 20\n' "$got" | sed 's/^/        /'
    exit 1
fi

echo "  [PASS] assign_to_call_result: writing to a call's result is a type error; writes through a returned pointer still land"
