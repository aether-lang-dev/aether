#!/bin/sh
# An actor state field's C type comes from its type and its use, never from
# its name (#2466).
#
# Every state field whose name ended in `_ref` was emitted as `void*`, so
# `state self_ref = 0` used as a number failed in the C compiler ("invalid
# operands to binary *"). The suffix stood in for one real case: a field
# initialised with `0` that later holds an actor reference, assigned from
# outside (`a.next = b`) and sent to (`next ! Msg {}`). That case is now
# recognised by the use itself, so it works under any name, and a `_ref`
# name used as a number stays a number.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] actor_state_ref_name_not_type: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
message Inc { n: int }
message Get {}
message Tick { n: int }

actor Counter {
    state self_ref = 0
    receive {
        Inc(n) -> { self_ref = self_ref + n * 3 }
        Get() -> { reply self_ref * 2 }
    }
}

// `next` holds an actor reference without a `_ref` in its name.
actor Node {
    state next = 0
    state hops = 0
    receive {
        Tick(n) -> {
            hops = hops + 1
            if n > 0 { next ! Tick { n: n - 1 } }
        }
        Get() -> { reply hops }
    }
}

main() {
    c = spawn(Counter())
    c ! Inc { n: 5 }
    v = c ? Get {}
    println("v ${v}")

    a = spawn(Node())
    b = spawn(Node())
    a.next = b
    b.next = a
    a ! Tick { n: 5 }
    wait_for_idle()
    ha = a ? Get {}
    hb = b ? Get {}
    println("hops ${ha} ${hb}")
}
AE

want="v 30
hops 3 3"
got="$("$AE" run "$tmp/main.ae" 2>&1 | tr -d '\r' | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] actor_state_ref_name_not_type: wrong output"
    printf 'got:\n%s\nwant:\n%s\n' "$got" "$want" | sed 's/^/        /'
    exit 1
fi

# The reference uses outside a receive arm's bare send: a field set from
# outside and only forwarded as a `ptr` message field (`back`), one sent
# through from outside (`h.target ! Go {}`), and `my_ref` used as a number,
# which spawn no longer overwrites with the actor's address, and a
# `my_ref` only passed to a `ptr` parameter, which spawn still sets.
cat > "$tmp/uses.ae" <<'AE'
message Go {}
message Fwd { to: ptr }
message Get {}

actor Sink {
    state hits = 0
    receive {
        Go() -> { hits = hits + 1 }
        Fwd(to) -> {
            hits = hits + 10
            to ! Go {}
        }
    }
}

actor Relay {
    state peer = 0
    state back = 0
    receive {
        Go() -> { peer ! Fwd { to: back } }
    }
}

actor Holder {
    state target = 0
    receive {
        Go() -> { }
    }
}

// `my_ref` used as a reference only by passing it to a `ptr` parameter:
// spawn still sets it to the actor's own address.
message Hello { from: ptr }
message Intro { to: ptr }

actor Greeter {
    state my_ref = 0
    state greeted = 0
    receive {
        Intro(to) -> { introduce(to, my_ref) }
        Go() -> { greeted = greeted + 1 }
        Get() -> { reply greeted }
    }
}

actor Host {
    state hellos = 0
    receive {
        Hello(from) -> {
            hellos = hellos + 1
            from ! Go {}
        }
        Get() -> { reply hellos }
    }
}

introduce(to: ptr, me: ptr) {
    to ! Hello { from: me }
}

actor Tally {
    state my_ref = 4
    receive {
        Go() -> { my_ref = my_ref * 10 + 2 }
        Get() -> { reply my_ref }
    }
}

main() {
    s = spawn(Sink())
    t = spawn(Sink())
    r = spawn(Relay())
    r.peer = s
    r.back = t
    r ! Go {}
    h = spawn(Holder())
    h.target = t
    h.target ! Go {}
    k = spawn(Tally())
    k ! Go {}
    g = spawn(Greeter())
    ho = spawn(Host())
    g ! Intro { to: ho }
    wait_for_idle()
    m = k ? Get {}
    gh = g ? Get {}
    hh = ho ? Get {}
    println("hits ${s.hits} ${t.hits} tally ${m} greeted ${gh} ${hh}")
}
AE
got="$("$AE" run "$tmp/uses.ae" 2>&1 | tr -d '\r' | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
if [ "$got" != "hits 10 2 tally 42 greeted 1 1" ]; then
    echo "  [FAIL] actor_state_ref_name_not_type: a reference field used from outside the actor"
    printf 'got:\n%s\nwant:\nhits 10 2 tally 42 greeted 1 1\n' "$got" | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] actor_state_ref_name_not_type: a *_ref field used as a number is a number; a send target is a reference under any name"
