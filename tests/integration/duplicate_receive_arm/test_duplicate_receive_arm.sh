#!/bin/sh
# A second receive arm for a message the actor already receives is refused
# by the type checker (#2654).
#
# A receive arm matches by the message alone: its pattern binds fields and
# takes no guard, and a message is dispatched to one handler per message
# type. Two arms for `Job` in one actor compiled to two definitions of
# `Worker_handle_Job`, which gcc refused ("redefinition"). The second arm,
# in the same receive block or another one, can never run, so `ae check`
# reports it, naming the arm that already receives the message. Arms for
# different messages stay legal.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] duplicate_receive_arm: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0

expect_error() {  # file, fixed string, label
    out="$(cd "$tmp" && AETHER_HOME="$ROOT" "$AE" check "$1" 2>&1)"
    if ! printf '%s' "$out" | grep -qF "$2"; then
        echo "  [FAIL] duplicate_receive_arm: $3"
        printf '%s\n' "$out" | grep -E "^error|-->" | head -4 | sed 's/^/        /'
        fail=1
    fi
}

# The issue's shape: two arms for one message in one receive block.
cat > "$tmp/same.ae" <<'AE'
message Job { n: int }

actor Worker {
    state seen = 0

    receive {
        Job(n) -> {
            seen = seen + n
        }
        Job(n) -> {
            seen = seen + 1
        }
    }
}

main() {
    w = spawn(Worker())
    w ! Job { n: 7 }
    wait_for_idle()
}
AE
expect_error same.ae "this receive arm for 'Job' can never run: the arm for 'Job' at line 7 already receives every 'Job' message of actor 'Worker'" \
    "a second arm for the same message was not refused"
expect_error same.ae "same.ae:10:" "the refusal does not point at the second arm"

# The second arm in another receive block of the same actor.
cat > "$tmp/blocks.ae" <<'AE'
message Job { n: int }
message Other { n: int }

actor Worker {
    state seen = 0
    receive {
        Job(n) -> {
            seen = seen + n
        }
        Other(n) -> {
            seen = seen + 1
        }
    }
    receive {
        Job(n: k) -> {
            seen = seen + k
        }
    }
}

main() {
    w = spawn(Worker())
    w ! Job { n: 7 }
    wait_for_idle()
}
AE
expect_error blocks.ae "this receive arm for 'Job' can never run: the arm for 'Job' at line 7" \
    "a second arm for the same message in another receive block was not refused"

# What stays legal: one arm per message, two actors with an arm each for
# the same message.
cat > "$tmp/ok.ae" <<'AE'
message Job { n: int }
message Other { n: int }

var total = 0

add(n: int) { total = total + n }

actor Worker {
    receive {
        Job(n) -> {
            add(n)
        }
        Other(n) -> {
            add(n * 10)
        }
    }
}

actor Helper {
    receive {
        Job(n) -> {
            add(n * 100)
        }
    }
}

main() {
    w = spawn(Worker())
    h = spawn(Helper())
    w ! Job { n: 1 }
    w ! Other { n: 2 }
    h ! Job { n: 3 }
    wait_for_idle()
    println("${total}")
}
AE
got="$(cd "$tmp" && AETHER_HOME="$ROOT" "$AE" run ok.ae 2>&1 | tr -d '\r' | tail -1)"
if [ "$got" != "321" ]; then
    echo "  [FAIL] duplicate_receive_arm: a legal actor was refused or misread (got '$got')"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] duplicate_receive_arm: a second arm for a received message is refused; one arm per message passes"
fi
exit $fail
