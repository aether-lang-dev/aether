#!/bin/sh
# A capturing closure bound to a local frees its env exactly once at scope
# exit, unless the value leaves the scope (#2480); a closure handed to a new
# owner is freed by that owner, and a receive arm is a scope (#2494, #2498).
#
# The scope-exit free was pushed only when closure_var_map had no entry for
# the name, and discover_closures fills that map for every closure binding
# before any statement is emitted, so it was never pushed: every call leaked
# the env and the promoted cells it holds. leaks(1) did not notice, since a
# dead stack slot still pointing at the env makes it look reachable.
#
# So this counts instead. env_count.h is force-included into the generated C
# (through the probe project's [build] cflags) and hooks the env and cell
# allocations and every free: a freed block is poisoned and kept, so a read
# through a dangling env gives a wrong value or a fault, and a second free is
# counted.
#
#   owned.ae     closures that never leave their scope, in every shape the
#                fix frees: all envs and cells freed, none twice.
#   escaping.ae  closures that are returned, aliased, stored, kept by a
#                callee or captured by a kept closure: the values are still
#                right after the declaring scope ended, and nothing is freed
#                twice (the list that owns some of them frees them too).
#   handover.ae  closures handed to another owner (#2494): a function's
#                returned closure bound by the caller, and envs that captured
#                another closure, wherever they went: all freed, none twice.
#   actor_arm.ae a receive arm is a scope (#2494, #2498): its defer runs, the
#                closures and cells of every message are freed, a struct
#                stored into state keeps its string.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] closure_local_env_free: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

fail=0

# run_probe <name>: builds <name>.ae with the counter, leaves its stdout in
# $tmp/<name>/out.txt and its ENVCOUNT line in $tmp/<name>/count.txt.
run_probe() {
    d="$tmp/$1"
    mkdir -p "$d"
    cp "$SCRIPT_DIR/$1.ae" "$d/probe.ae"
    cp "$SCRIPT_DIR/env_count.h" "$d/env_count.h"
    printf '[build]\ncflags = "-include env_count.h"\n' > "$d/aether.toml"
    if ! (cd "$d" && "$AE" build probe.ae -o probe) > "$d/build.log" 2>&1; then
        echo "  [FAIL] closure_local_env_free: $1.ae did not build"
        sed 's/^/        /' "$d/build.log" | head -20
        fail=1
        return 1
    fi
    (cd "$d" && ./probe) > "$d/out.txt" 2> "$d/err.txt"
    rc=$?
    tr -d '\r' < "$d/err.txt" | grep '^ENVCOUNT' > "$d/count.txt"
    if [ $rc -ne 0 ]; then
        echo "  [FAIL] closure_local_env_free: $1 exited $rc"
        tr -d '\r' < "$d/out.txt" | sed 's/^/        /' | tail -5
        fail=1
        return 1
    fi
    return 0
}

# field <name> <key>: one number from the probe's ENVCOUNT line.
field() {
    sed -n "s/.* $2=\([0-9]*\).*/\1/p" "$tmp/$1/count.txt"
}

if run_probe owned; then
    out="$(tr -d '\r' < "$tmp/owned/out.txt")"
    envs="$(field owned envs)"
    if [ "$out" != "owned ok 8057" ]; then
        echo "  [FAIL] closure_local_env_free: owned closures computed the wrong values"
        printf '%s\n' "$out" | sed 's/^/        /' | tail -5
        fail=1
    elif [ -z "$envs" ] || [ "$envs" -eq 0 ]; then
        echo "  [FAIL] closure_local_env_free: the counter saw no env (env_count.h not applied?)"
        fail=1
    elif [ "$(field owned env_live)" != 0 ] || [ "$(field owned cell_live)" != 0 ] ||
         [ "$(field owned double_frees)" != 0 ]; then
        echo "  [FAIL] closure_local_env_free: closures kept in their scope leak or double free"
        sed 's/^/        /' "$tmp/owned/count.txt"
        fail=1
    else
        echo "  [PASS] closure_local_env_free: $envs local envs and their cells freed once each"
    fi
fi

if run_probe escaping; then
    out="$(tr -d '\r' < "$tmp/escaping/out.txt")"
    if [ "$out" != "escaping ok" ]; then
        echo "  [FAIL] closure_local_env_free: an escaping closure lost its env"
        printf '%s\n' "$out" | sed 's/^/        /' | tail -5
        fail=1
    elif [ ! -s "$tmp/escaping/count.txt" ] || [ "$(field escaping double_frees)" != 0 ]; then
        echo "  [FAIL] closure_local_env_free: an escaping closure's env was freed twice"
        sed 's/^/        /' "$tmp/escaping/count.txt"
        fail=1
    else
        echo "  [PASS] closure_local_env_free: returned, stored and kept closures keep their envs"
    fi
fi

# expect_clean <name> <expected output> <what>: the probe printed exactly
# the expected output and freed every env and cell it built, none twice.
expect_clean() {
    run_probe "$1" || return
    out="$(tr -d '\r' < "$tmp/$1/out.txt")"
    envs="$(field "$1" envs)"
    if [ "$out" != "$2" ]; then
        echo "  [FAIL] closure_local_env_free: $1 computed the wrong values"
        printf '%s\n' "$out" | sed 's/^/        /' | tail -5
        fail=1
    elif [ -z "$envs" ] || [ "$envs" -eq 0 ]; then
        echo "  [FAIL] closure_local_env_free: $1 built no env the counter saw"
        fail=1
    elif [ "$(field "$1" env_live)" != 0 ] || [ "$(field "$1" cell_live)" != 0 ] ||
         [ "$(field "$1" double_frees)" != 0 ]; then
        echo "  [FAIL] closure_local_env_free: $1 leaks or double frees"
        sed 's/^/        /' "$tmp/$1/count.txt"
        fail=1
    else
        echo "  [PASS] closure_local_env_free: $3 ($envs envs, each freed once)"
    fi
}

expect_clean handover "handover ok" "returned and captured closures are freed by their new owner"
expect_clean actor_arm "hi 3
bye 3
kept kept! count 1 total 3725" "a receive arm runs its defer and frees what each message built"

exit $fail
