#!/bin/sh
# A user function whose name starts with `spawn_` is an ordinary call
# (#2126). Codegen took the prefix for the generated actor spawner
# `spawn_Name(core)` and emitted `spawn_once(1, 2, p)` as `spawn_once(1)`,
# dropping the rest of the arguments without a word; the C compiler then
# complained about the arity. The actor spawner itself keeps working.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] spawn_prefixed_function: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT

cat > "$tmp/main.ae" <<'AE'
extern strlen(s: string) -> int

message Ping {}

actor Once {
    state n = 0
    receive { Ping() -> { n = n + 1 } }
}

f(x: int, n: int, p: ptr) -> int { return x * 10 + n }
spawn_once(x: int, n: int, p: ptr) -> int { return x * 10 + n }
spawn_helper(a: int, b: int) -> int { return a + b }

main() {
    block = null as ptr
    o = spawn_Once(-1)
    o ! Ping {}
    println("${f(1, 2, block)} ${spawn_once(1, 2, block)} ${spawn_helper(3, 4)}")
}
AE
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/main.ae" 2>&1 | tail -1)"
if [ "$got" != "12 12 7" ]; then
    echo "  [FAIL] spawn_prefixed_function: a user spawn_* function lost its arguments (got '$got')"
    exit 1
fi
AETHER_HOME="$ROOT" "$AETHERC" "$tmp/main.ae" "$tmp/out.c" >/dev/null 2>&1
if ! grep -q "spawn_once(1, 2, block)" "$tmp/out.c"; then
    echo "  [FAIL] spawn_prefixed_function: expected the full call 'spawn_once(1, 2, block)' in the C"
    exit 1
fi
echo "  [PASS] spawn_prefixed_function: a user function named spawn_* is an ordinary call; the actor spawner still works"
exit 0
