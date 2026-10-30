#!/bin/sh
# #2297: structs cross a binary import, module state lives once, and on the
# shared runtime a library's panic reaches the program's catch everywhere.
#
# A library's catalog (schema 1.3) carries the structs its exports use and
# each export's signature as Aether source, so the interface `ae` synthesizes
# for an importer declares the same structs and the same typed externs:
# a struct passes by value, a `*Model` reaches fields, a function-pointer
# field keeps the layout. Before, a typed pointer arrived as an opaque `ptr`
# and a function taking or returning a struct was not exported at all.
#
# Two passes over the same sources:
#   static runtime  each library carries its own copy of libaether, and on
#                   every platform (#3481) the copies stay apart: neither
#                   may free the other's strings, so importing the engine,
#                   whose *Model holds one, is refused, naming the rebuild
#                   (#2931).
#   shared runtime  `--shared-runtime`: the libraries link aether.dll /
#                   libaether.so / libaether.dylib, their catalogs say so
#                   (schema 1.5), and the host follows. One runtime, so the
#                   panic crosses on every platform.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] binary_import_structs: ae not built"; exit 0; }

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) SO_EXT=".dll";   EXE=".exe" ;;
    Darwin)                          SO_EXT=".dylib"; EXE="" ;;
    *)                               SO_EXT=".so";    EXE="" ;;
esac

fail() {
    echo "  [FAIL] binary_import_structs: $1"
    if [ -n "$2" ] && [ -f "$2" ]; then sed 's/^/    /' "$2" | head -30; fi
    exit 1
}

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK" || true' EXIT

# run_pass <dir> <ae build flags for the libraries> <label>
run_pass() {
    dir="$1"; flags="$2"; label="$3"
    mkdir -p "$dir"
    cp "$SCRIPT_DIR/engine.ae" "$SCRIPT_DIR/script.ae" "$SCRIPT_DIR/app.ae" \
       "$SCRIPT_DIR/app_panic.ae" "$dir/"
    cd "$dir"

    AETHER_HOME="$ROOT" "$AE" build --emit=lib $flags engine.ae -o "libengine$SO_EXT" >engine.log 2>&1 \
        || fail "$label: ae build --emit=lib engine.ae" engine.log
    rm -f engine.ae   # every importer resolves the engine to the library from here on

    INFO="$(AETHER_HOME="$ROOT" "$AE" lib-info "./libengine$SO_EXT" 2>&1)" || { echo "$INFO"; fail "$label: ae lib-info"; }
    echo "$INFO" | grep -q "Structs:[[:space:]]*2" || { echo "$INFO"; fail "$label: lib-info does not count 2 structs"; }
    echo "$INFO" | grep -q "update: fn(ptr, float) -> int" || { echo "$INFO"; fail "$label: the fn-pointer field is not recorded"; }
    echo "$INFO" | grep -q "as: model_new(name: string) -> \*Model" \
        || { echo "$INFO"; fail "$label: the typed-pointer source signature is missing"; }
    echo "$INFO" | grep -q "as: make_adder(k: int) -> fn" \
        || { echo "$INFO"; fail "$label: a closure-returning export is missing or mis-signed"; }

    AETHER_HOME="$ROOT" "$AE" build --emit=lib $flags script.ae -o "libscript$SO_EXT" >script.log 2>&1 \
        || fail "$label: ae build --emit=lib script.ae (importing the engine binary)" script.log
    rm -f script.ae

    OUT="$(AETHER_HOME="$ROOT" "$AE" run app.ae 2>run.log)" || { echo "$OUT"; fail "$label: ae run app.ae" run.log; }
    echo "$OUT" | grep -q "^OK" || { echo "$OUT"; fail "$label: ae run app.ae did not pass its checks"; }

    AETHER_HOME="$ROOT" "$AE" build app.ae -o app >app.log 2>&1 || fail "$label: ae build app.ae" app.log
    OUT2="$(./app$EXE 2>&1)" || { echo "$OUT2"; fail "$label: the built host failed"; }
    echo "$OUT2" | grep -q "^OK" || { echo "$OUT2"; fail "$label: the built host did not pass its checks"; }
    cd "$WORK"
}

panic_check() {
    dir="$1"; label="$2"
    cd "$dir"
    OUT3="$(AETHER_HOME="$ROOT" "$AE" run app_panic.ae 2>panic.log)" \
        || { echo "$OUT3"; fail "$label: a library panic did not reach the host's catch" panic.log; }
    echo "$OUT3" | grep -q "^OK" || { echo "$OUT3"; fail "$label: app_panic.ae did not pass"; }
    cd "$WORK"
}

# ---- static runtime ----
mkdir -p "$WORK/static"
cp "$SCRIPT_DIR/engine.ae" "$SCRIPT_DIR/app.ae" "$WORK/static/"
cd "$WORK/static"
AETHER_HOME="$ROOT" "$AE" build --emit=lib engine.ae -o "libengine$SO_EXT" >engine.log 2>&1 \
    || fail "static runtime: ae build --emit=lib engine.ae" engine.log
rm -f engine.ae
if AETHER_HOME="$ROOT" "$AE" run app.ae >run.log 2>&1; then
    fail "static runtime: importing a library whose *Model holds a string was not refused" run.log
fi
grep -q "model_new\` passes \*Model, which holds strings" run.log \
    || fail "static runtime: the refusal does not name model_new and *Model" run.log
grep -q "ae build --emit=lib --shared-runtime" run.log \
    || fail "static runtime: the refusal does not name the rebuild" run.log
cd "$WORK"

# ---- shared runtime (where this toolchain built one) ----
if [ -d "$ROOT/build/shared" ]; then
    run_pass "$WORK/shared" "--shared-runtime" "shared runtime"
    cd "$WORK/shared"
    INFO="$(AETHER_HOME="$ROOT" "$AE" lib-info "./libengine$SO_EXT" 2>&1)"
    # 1.11: every catalog has the outcome slot (#2956) and the host ABI, the runtime ABI and the duties (#2957); `runtime` (1.5) is in it.
    echo "$INFO" | grep -q "Schema:[[:space:]]*1\.11" || { echo "$INFO"; fail "shared runtime: schema is not 1.11"; }
    echo "$INFO" | grep -q "Runtime:[[:space:]]*shared" || { echo "$INFO"; fail "shared runtime: the catalog does not say so"; }
    cd "$WORK"
    panic_check "$WORK/shared" "shared runtime"
    echo "  [PASS] binary_import_structs: structs, typed pointers, one module state; on the shared runtime a library panic is caught by the host"
else
    echo "  [PASS] binary_import_structs: structs, typed pointers and one module state (no shared runtime built here)"
fi
