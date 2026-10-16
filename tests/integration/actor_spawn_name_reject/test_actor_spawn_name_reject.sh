#!/bin/sh
# A function named like an actor's spawn function is an error naming the
# actor (#2804): spawn_<Actor> is API, the function a C host embedding the
# program calls, so the generated C keeps that name. Before, the program
# compiled to C with two definitions of one symbol.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE_TMP="$(mktemp -d)"
trap 'rm -rf "$AE_TMP"' EXIT

cat > "$AE_TMP/bad.ae" << 'AE'
message Ping {}
actor Counter {
    state n = 0
    receive { Ping() -> { n = n + 1 } }
}

spawn_Counter() -> int {
    return 1
}

main() {
    _c = spawn(Counter())
    println("${spawn_Counter()}")
}
AE
if out=$(AETHER_HOME="$ROOT" "$ROOT/build/ae" build "$AE_TMP/bad.ae" -o "$AE_TMP/bad" 2>&1); then
    echo "  [FAIL] actor_spawn_name_reject: a function named spawn_Counter compiled"
    exit 1
fi
if ! echo "$out" | grep -q "function 'spawn_Counter' takes the name of the spawn function of actor 'Counter'"; then
    echo "  [FAIL] actor_spawn_name_reject: the error does not name the actor"
    echo "$out" | sed 's/^/    /' | head -10
    exit 1
fi
if echo "$out" | grep -q "conflicting types"; then
    echo "  [FAIL] actor_spawn_name_reject: the C compiler saw it first"
    exit 1
fi
echo "  [PASS] actor_spawn_name_reject"
