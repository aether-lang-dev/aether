#!/bin/sh
# #3574: a diagnostic never loses the words that say what is wrong. The
# messages were formatted into fixed buffers (160 to 2048 bytes), and a long
# name formatted ahead of the explanation cut the explanation off: a local,
# a field, a function or a module of 600 characters left a message of the
# name and nothing after it. The messages are as long as they are now.
#
# One case per family of messages, each with a 600-character name ahead of
# the words it checks: the checker's narrowing, message-field and enum-arm
# messages, the free check, the protocol check, slice ownership, the escape
# check, @derive, the missing-return check, @quotable, the code generator's
# lane, actor-state and array-size messages, and the module loader's
# unresolved import.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=diagnostic_long_names
[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
AETHER_HOME="$ROOT"
export AETHER_HOME
fails=0

L=$(awk 'BEGIN { s = "n"; while (length(s) < 600) s = s "x"; print s }')
# Where a family's buffer held 600 characters and its words (1024 or 2048
# bytes), the name is long enough to pass it.
L1100=$(awk 'BEGIN { s = "n"; while (length(s) < 1100) s = s "x"; print s }')
L2100=$(awk 'BEGIN { s = "n"; while (length(s) < 2100) s = s "x"; print s }')

# says <dir> <how> <text>: `ae <how>` of $TMP/<dir>/main.ae fails and its
# output has <text> (the words after the long name).
says() {
    if [ "$2" = build ]; then
        out=$(cd "$TMP/$1" && "$AE" build main.ae -o main 2>&1)
    else
        out=$(cd "$TMP/$1" && "$AE" check main.ae 2>&1)
    fi
    if [ $? -eq 0 ] || ! printf '%s' "$out" | grep -qF -- "$3"; then
        echo "  [FAIL] $NAME: $1: not \"$3\":"
        printf '%s\n' "$out" | grep -E "error|warning" | cut -c1-120 | head -4 | sed 's/^/        /'
        fails=$((fails + 1))
    fi
}

# case_of <dir> [name]: the program on stdin, with @L@ for the long name (or
# [name]).
case_of() {
    mkdir -p "$TMP/$1"
    sed "s/@L@/${2:-$L}/g" > "$TMP/$1/main.ae"
}

case_of narrowing <<'AE'
main() {
    @L@ = 1
    @L@ = 4000000000
    println("${@L@}")
}
AE
says narrowing check "its type was inferred as 32-bit int from its initializer"

case_of message_field <<'AE'
message M { @L@: int }
actor A {
    receive {
        M(@L@) -> {}
    }
}
main() {
    a = spawn(A())
    a ! M { @L@: "text" }
}
AE
says message_field check "of message 'M': expected int"

case_of enum_arm <<'AE'
enum @L@ { Red, Green }
pick(c: @L@) -> int {
    match c {
        Red -> { return 1 }
        Green -> { return 2 }
        _ -> { return 3 }
    }
}
main() { println("${pick(Red)}") }
AE
out=$(cd "$TMP/enum_arm" && "$AE" check main.ae 2>&1)
printf '%s' "$out" | grep -qF "is already handled, so the \`_\` arm cannot run" ||
    { echo "  [FAIL] $NAME: enum_arm: the warning lost its words:"; printf '%s\n' "$out" | grep warning | cut -c1-120 | head -3; fails=$((fails + 1)); }

case_of free_check <<'AE'
import std.bytes
main() {
    @L@ = bytes.new(1)
    bytes.free(@L@)
    bytes.free(@L@)
}
AE
says free_check check "is freed again: it was freed at line"

case_of protocol <<'AE'
message Open {}
message Write {}
protocol W {
    start -> Open -> open
    open -> Write -> open
}
actor F implements W {
    receive {
        Open() -> {}
        Write() -> {}
    }
}
main() {
    @L@ = spawn(F())
    @L@ ! Write {}
}
AE
says protocol check "is in state 'start'"

mkdir -p "$TMP/slice_own/lib/qm"
cat > "$TMP/slice_own/lib/qm/module.ae" <<'AE'
keeper(s: @keep int[]) {
    println("kept ${s.len}")
}
AE
case_of slice_own <<'AE'
import qm
pass(@L@: int[]) {
    qm.keeper(@L@)
}
main() { pass(make([]int, 2)) }
AE
says slice_own check "is a parameter, which borrows the caller's slice"

case_of escape "$L1100" <<'AE'
struct H {
    data: int[]
}
fill(@L@: *H) {
    let a: int[3] = [1, 2, 3]
    @L@.data = a[0..2]
}
main() {
    let h: H = H { data: make([]int, 1) }
    fill(&h)
}
AE
says escape check "the storage ends when"

case_of derive <<'AE'
@derive(eq)
struct @L@ {
    handle: ptr,
    n: int
}
main() {}
AE
says derive check "has an unsupported type"

case_of missing_return <<'AE'
@L@(n: int) -> int {
    if n > 0 {
        return 1
    }
}
main() { println("${@L@(1)}") }
AE
says missing_return check "whose result is int"

case_of quotable "$L1100" <<'AE'
import std.quote
take(t: quote.Tree) {}
@quotable
@L@(x: float) -> float { return @L@(x) }
main() {
    take() quote | r: float | { return @L@(r) }
}
AE
says quotable check "reaches itself"

case_of actor_state <<'AE'
message Go {}
actor A {
    state @L@ = 0
    receive {
        Go() -> {
            f = || { @L@ = 1 }
            f()
        }
    }
}
main() {
    a = spawn(A())
    a ! Go {}
    wait_for_idle()
}
AE
says actor_state build "not supported (closures can't mutate actor state"

case_of array_size <<'AE'
main() {
    let @L@: int[2] = [1, 2]
    @L@ = [1, 2, 3]
    println("${@L@[0]}")
}
AE
says array_size build "which holds 2"

mkdir -p "$TMP/lane"
cat > "$TMP/lane/twice.ae" <<AE
import std.lanes
exports ($L)
$L(v: f32x8) -> f32x8 { return v + v }
AE
case_of lane <<'AE'
import std.lanes
@target("avx2") import twice as t8
main() { println("${lanes.sum8(t8.@L@(lanes.splat8(1.0)))}") }
AE
says lane build "passes eight-lane values between a module imported at @target"

case_of import "$L2100" <<'AE'
import @L@
main() {}
AE
says import check "no module of that name was found in"

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] $NAME: every family's message keeps its words past a 600-character name"
exit 0
