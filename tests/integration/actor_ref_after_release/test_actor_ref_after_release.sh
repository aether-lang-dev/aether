#!/bin/sh
# A send through an AetherActorRef taken from an actor that has been released
# is dropped and counted, even once the actor's block belongs to a new actor
# (#2527). A released block goes to the next spawn of its size; through the
# bare pointer a late send then reached the new actor. The new actor has an
# id of its own, which the reference compares.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] actor_ref_after_release: ae not built"; exit 0; }
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*|Windows_NT) EXE=".exe" ;; *) EXE="" ;; esac

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

if ! AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/probe.ae" -o "$TMPDIR/probe" \
        --extra "$SCRIPT_DIR/shim.c" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] actor_ref_after_release: build failed"
    sed 's/^/    /' "$TMPDIR/build.log" | head -20
    exit 1
fi
if ! "$TMPDIR/probe$EXE" >"$TMPDIR/run.log" 2>&1 \
   || ! grep -q "actor_ref_after_release passed" "$TMPDIR/run.log"; then
    echo "  [FAIL] actor_ref_after_release: the probe failed"
    sed 's/^/    /' "$TMPDIR/run.log" | head -20
    exit 1
fi
echo "  [PASS] actor_ref_after_release"
