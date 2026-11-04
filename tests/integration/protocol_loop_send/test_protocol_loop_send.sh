#!/bin/sh
# #3575: a send that its protocol allows in no state the reference may be in
# is refused where it is, in a loop as outside one. In a loop it compiled:
# the passes that do not report took the reference to unknown, the loop's
# head joined to unknown, and the walk that reports, from the head, refused
# nothing.
#
#   loop_send:  `w ! Write {}` on every turn, `w` in `start`: refused.
#   line_send:  the same send outside a loop: refused, as it always was.
#   loop_ok:    `w ! Open {}` first, then `w ! Write {}` on every turn: it
#               compiles and runs.
#   after_lost: a send wrong on every path, then a legal one, then one wrong
#               after it: the legal one leads where the protocol says, so the
#               third is refused too.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=protocol_loop_send

[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fails=0

# refused <dir> <count> <text>: `ae check` of $TMP/<dir>/main.ae fails with
# <count> protocol errors, one saying <text>.
refused() {
    out=$(cd "$TMP/$1" && AETHER_HOME="$ROOT" "$AE" check main.ae 2>&1)
    rc=$?
    n=$(printf '%s\n' "$out" | grep -c '^error\[E0600\]')
    if [ $rc -eq 0 ] || [ "$n" != "$2" ] || ! printf '%s' "$out" | grep -qF "$3"; then
        echo "  [FAIL] $NAME: $1: not refused $2 times with \"$3\":"
        printf '%s\n' "$out" | grep -E "^error|-->" | cut -c1-160 | head -8 | sed 's/^/        /'
        fails=$((fails + 1))
    fi
}

# runs <dir> <want>: `ae run` of $TMP/<dir>/main.ae prints <want>.
runs() {
    out=$(cd "$TMP/$1" && AETHER_HOME="$ROOT" "$AE" run main.ae 2>&1 | tr -d '\r')
    if [ "$out" != "$2" ]; then
        echo "  [FAIL] $NAME: $1: not \"$2\":"
        printf '%s\n' "$out" | cut -c1-160 | head -8 | sed 's/^/        /'
        fails=$((fails + 1))
    fi
}

# program <dir> <body of use(n: int)>: the protocol, the actor, and a
# function sending through `w`.
program() {
    mkdir -p "$TMP/$1"
    cat > "$TMP/$1/main.ae" <<AE
message Open {}
message Write {}

protocol W {
    start -> Open -> open
    open -> Write -> open
}

actor F implements W {
    state writes = 0
    receive {
        Open() -> { println("open") }
        Write() -> { writes = writes + 1 }
    }
}

use(n: int) {
    w = spawn(F())
$2
    wait_for_idle()
    println("writes \${w.writes}")
}

main() { use(3) }
AE
}

program loop_send '    i = 0
    while i < n {
        w ! Write {}
        i = i + 1
    }'
refused loop_send 1 "protocol 'W' does not allow 'Write' here: 'w' is in state 'start'"

program line_send '    w ! Write {}'
refused line_send 1 "protocol 'W' does not allow 'Write' here: 'w' is in state 'start'"

program loop_ok '    w ! Open {}
    i = 0
    while i < n {
        w ! Write {}
        i = i + 1
    }'
runs loop_ok "open
writes 3"

program after_lost '    w ! Write {}
    w ! Open {}
    w ! Open {}'
refused after_lost 2 "protocol 'W' does not allow 'Open' here: 'w' is in state 'open'"

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] $NAME: a send no state allows is refused in a loop as outside one"
exit 0
