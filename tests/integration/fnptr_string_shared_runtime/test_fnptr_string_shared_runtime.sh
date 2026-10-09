#!/bin/sh
# #2687: a program that calls a string function through a typed fn pointer
# links and runs against the shared runtime. The #2586 helpers in the
# generated C read and wrote the runtime's thread-local ownership mark
# directly, and on Windows a thread-local cannot be imported from a DLL: the
# link failed (or ld crashed on larger programs), which is what ae3d hits,
# since it links its programs and the scripts they load to one runtime DLL.
# The mark now stays inside the runtime, behind two functions.
#
# Skipped where this toolchain has no shared runtime (`make shared-runtime`).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

fail() { echo "  [FAIL] fnptr_string_shared_runtime: $1"; exit 1; }

if ! ls "$ROOT/build/shared/" 2>/dev/null | grep -q "aether"; then
    echo "  [SKIP] fnptr_string_shared_runtime: no shared runtime in build/shared"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

"$AE" build --shared-runtime "$SCRIPT_DIR/prog.ae" -o "$TMP/prog" > "$TMP/build.log" 2>&1 \
    || { cat "$TMP/build.log"; fail "the program did not build against the shared runtime"; }
# Windows finds the runtime DLL beside the program or on PATH.
PATH="$ROOT/build/shared:$PATH"
LD_LIBRARY_PATH="$ROOT/build/shared${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
DYLD_LIBRARY_PATH="$ROOT/build/shared${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
export PATH LD_LIBRARY_PATH DYLD_LIBRARY_PATH
out="$("$TMP/prog" 2>&1)" || { echo "$out"; fail "the program did not run"; }
[ "$out" = "7" ] || { echo "$out"; fail "expected 7"; }

echo "  [PASS] fnptr_string_shared_runtime: a string call through a fn pointer links and runs on the shared runtime"
