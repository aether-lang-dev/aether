#!/bin/sh
# Every spawned actor sits on the 64-byte boundary its generated struct is
# declared with (#2485).
#
# codegen_actor.c emits __attribute__((aligned(64))) on each actor struct,
# but scheduler_spawn_actor allocated through aether_numa_alloc, which is
# plain malloc unless libnuma is in use: 16 bytes. Using such an actor is
# undefined behaviour and loses the cache-line isolation the attribute is
# for. probe.ae spawns 120 actors of three sizes and asks shim.c whether
# each address is a multiple of 64; before the fix most were not.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] actor_alignment: $AE not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

if ! AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/probe.ae" -o "$TMPDIR/probe" \
        --extra "$SCRIPT_DIR/shim.c" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] actor_alignment: build failed"
    sed 's/^/    /' "$TMPDIR/build.log" | head -15
    exit 1
fi

if ! "$TMPDIR/probe" >"$TMPDIR/run.log" 2>&1; then
    echo "  [FAIL] actor_alignment: probe exited non-zero"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi

if ! grep -q "spawned=120 misaligned=0" "$TMPDIR/run.log" ||
   ! grep -q "actor_alignment passed" "$TMPDIR/run.log"; then
    echo "  [FAIL] actor_alignment: actors off the 64-byte boundary"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi

echo "  [PASS] actor_alignment"
