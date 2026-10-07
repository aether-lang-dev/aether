#!/bin/sh
# #2454: a receive pattern binding named like a state field.
#
# Inside an actor, state is reached by its bare name, so in
# `state v = 100 ... M(v) -> { v += 1 }` the arm cannot have both. Codegen
# resolved `v` to the state field: the message's value was never read, and
# the state was changed instead (the arm printed 101 where 6 was due). It is
# refused at the binding now, and the rename it suggests works.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] actor_receive_binding_state_name: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

cat > "$tmp/clash.ae" <<'AE'
message M { v: int }
actor A {
    state v = 100
    receive {
        M(v) -> {
            v += 1
            println("got ${v}")
        }
    }
}
main() {
    a = spawn(A())
    a ! M { v: 5 }
    sleep(200)
}
AE
out="$("$AE" run "$tmp/clash.ae" 2>&1)"
if ! printf '%s\n' "$out" | grep -q "receive pattern binds 'v', which is also a state field of this actor"; then
    echo "  [FAIL] actor_receive_binding_state_name: the clash was not refused"
    printf '%s\n' "$out" | head -5 | sed 's/^/        /'
    exit 1
fi
if ! printf '%s\n' "$out" | grep -q 'M(v: new_v)'; then
    echo "  [FAIL] actor_receive_binding_state_name: the error does not show the rename"
    exit 1
fi

# The suggested rename: the arm reads the message's value and the state
# keeps its own.
cat > "$tmp/renamed.ae" <<'AE'
message M { v: int }
message Show {}
actor A {
    state v = 100
    receive {
        M(v: new_v) -> {
            new_v += 1
            println("got ${new_v}")
        }
        Show() -> {
            println("state ${v}")
        }
    }
}
main() {
    a = spawn(A())
    a ! M { v: 5 }
    a ! Show {}
    sleep(200)
}
AE
got="$("$AE" run "$tmp/renamed.ae" 2>&1 | tr -d '\r' | grep -E '^(got|state) ')"
want="got 6
state 100"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] actor_receive_binding_state_name: the renamed binding"
    printf 'got:\n%s\nwant:\n%s\n' "$got" "$want" | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] actor_receive_binding_state_name: a binding named like a state field is refused; the rename reads the message"
