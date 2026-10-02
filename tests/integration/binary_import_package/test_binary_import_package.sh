#!/bin/sh
# #2297: a package of modules built as one library, imported by module.
#
# `ae build --emit=lib --package gamekit` builds every module under
# src/gamekit/ (gamekit.core, gamekit.math) into libgamekit. Each module's
# exported functions get a stable wrapper, aether_gamekit_core__create, and
# the catalog (schema 1.4) says which module each belongs to; private ones
# (not in an exports list, or `_`-suffixed) stay out. A host's
# `import gamekit.core` with no source then resolves to the library.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] binary_import_package: ae not built"; exit 0; }

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] binary_import_package: Windows DLL hosting needs one shared runtime (#2297)"
        exit 0
        ;;
    Darwin) SO_EXT=".dylib" ;;
    *)      SO_EXT=".so" ;;
esac

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK" || true' EXIT
cp -R "$SCRIPT_DIR/src" "$SCRIPT_DIR/app.ae" "$WORK/"
cd "$WORK"

fail() {
    echo "  [FAIL] binary_import_package: $1"
    if [ -n "$2" ] && [ -f "$2" ]; then sed 's/^/    /' "$2" | head -30; fi
    exit 1
}

AETHER_HOME="$ROOT" "$AE" build --emit=lib --package gamekit -o "libgamekit$SO_EXT" >lib.log 2>&1 \
    || fail "ae build --emit=lib --package gamekit" lib.log
[ -f "libgamekit$SO_EXT" ] || fail "libgamekit$SO_EXT not produced"

INFO="$(AETHER_HOME="$ROOT" "$AE" lib-info "./libgamekit$SO_EXT" 2>&1)" || fail "ae lib-info"
echo "$INFO" | grep -q "Schema:[[:space:]]*1\.4" || { echo "$INFO"; fail "schema is not 1.4"; }
echo "$INFO" | grep -q "c_symbol: aether_gamekit_core__create" \
    || { echo "$INFO"; fail "core.create is not exported as aether_gamekit_core__create"; }
echo "$INFO" | grep -q "module: gamekit.math" || { echo "$INFO"; fail "no function is attributed to gamekit.math"; }
case "$INFO" in
    *secret*)       echo "$INFO"; fail "core.secret is not in core's exports list but was exported" ;;
    *half_*)        echo "$INFO"; fail "math.half_ is private by its suffix but was exported" ;;
    *HIDDEN_LIMIT*) echo "$INFO"; fail "core.HIDDEN_LIMIT is not exported but was cataloged" ;;
esac

rm -rf src   # every import of gamekit.* now resolves to the library

OUT="$(AETHER_HOME="$ROOT" "$AE" run app.ae 2>run.log)" || { echo "$OUT"; fail "ae run app.ae" run.log; }
echo "$OUT" | grep -q "^OK$" || { echo "$OUT"; fail "ae run app.ae did not pass its checks"; }

AETHER_HOME="$ROOT" "$AE" build app.ae -o app >app.log 2>&1 || fail "ae build app.ae" app.log
OUT2="$(./app 2>&1)" || { echo "$OUT2"; fail "the built host failed"; }
echo "$OUT2" | grep -q "^OK$" || { echo "$OUT2"; fail "the built host did not pass its checks"; }

echo "  [PASS] binary_import_package: a package library serves its modules by name"
