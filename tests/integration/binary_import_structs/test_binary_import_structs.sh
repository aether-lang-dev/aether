#!/bin/sh
# #2297: structs cross a binary import, and module state lives once.
#
# A library's catalog (schema 1.3) carries the structs its exports use and
# each export's signature as Aether source, so the interface `ae` synthesizes
# for an importer declares the same structs and the same typed externs:
# a struct passes by value, a `*Model` reaches fields, a function-pointer
# field keeps the layout. Before, a typed pointer arrived as an opaque `ptr`
# and a function taking or returning a struct was not exported at all.
#
# Also pins what the issue needs from the link model: a second binary
# library (script) importing the engine, and the host importing both, share
# the engine's one module state, and a panic inside the engine reaches the
# host's catch.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] binary_import_structs: ae not built"; exit 0; }

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        # A -static DLL carries its own copy of the runtime, and PE has no
        # symbol interposition to merge it with the host's: a panic in the
        # library would not reach the host's catch. Tracked in #2297.
        echo "  [SKIP] binary_import_structs: Windows DLL hosting needs one shared runtime (#2297)"
        exit 0
        ;;
    Darwin) SO_EXT=".dylib" ;;
    *)      SO_EXT=".so" ;;
esac

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK" || true' EXIT
cp "$SCRIPT_DIR/engine.ae" "$SCRIPT_DIR/script.ae" "$SCRIPT_DIR/app.ae" "$WORK/"
cd "$WORK"

fail() {
    echo "  [FAIL] binary_import_structs: $1"
    if [ -n "$2" ] && [ -f "$2" ]; then sed 's/^/    /' "$2" | head -30; fi
    exit 1
}

AETHER_HOME="$ROOT" "$AE" build --emit=lib engine.ae -o "libengine$SO_EXT" >engine.log 2>&1 \
    || fail "ae build --emit=lib engine.ae" engine.log
rm -f engine.ae   # every importer resolves the engine to the library from here on

INFO="$(AETHER_HOME="$ROOT" "$AE" lib-info "./libengine$SO_EXT" 2>&1)" || fail "ae lib-info"
echo "$INFO" | grep -q "Schema:[[:space:]]*1\.3" || { echo "$INFO"; fail "schema is not 1.3"; }
echo "$INFO" | grep -q "Structs:[[:space:]]*2" || { echo "$INFO"; fail "lib-info does not count 2 structs"; }
echo "$INFO" | grep -q "update: fn(ptr, float) -> int" || { echo "$INFO"; fail "the fn-pointer field is not recorded"; }
echo "$INFO" | grep -q "as: model_new(name: string) -> \*Model" \
    || { echo "$INFO"; fail "the typed-pointer source signature is missing"; }

AETHER_HOME="$ROOT" "$AE" build --emit=lib script.ae -o "libscript$SO_EXT" >script.log 2>&1 \
    || fail "ae build --emit=lib script.ae (importing the engine binary)" script.log
rm -f script.ae

OUT="$(AETHER_HOME="$ROOT" "$AE" run app.ae 2>run.log)" || { echo "$OUT"; fail "ae run app.ae" run.log; }
echo "$OUT" | grep -q "^OK$" || { echo "$OUT"; fail "ae run app.ae did not pass its checks"; }

AETHER_HOME="$ROOT" "$AE" build app.ae -o app >app.log 2>&1 || fail "ae build app.ae" app.log
OUT2="$(./app 2>&1)" || { echo "$OUT2"; fail "the built host failed"; }
echo "$OUT2" | grep -q "^OK$" || { echo "$OUT2"; fail "the built host did not pass its checks"; }

echo "  [PASS] binary_import_structs: structs, typed pointers and one module state across binary imports"
