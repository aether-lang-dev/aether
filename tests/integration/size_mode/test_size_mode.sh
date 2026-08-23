#!/bin/sh
# `ae build --size` produces a smaller artifact without breaking it.
#
# ae build had --quick (-O0 -g), --profile (-O2 -g -fno-omit-frame-pointer)
# and --coverage (-O0 -g --coverage) -- all debug-oriented -- and no mode
# pointing the other way. That mattered most on the cross path: `zig cc`
# emits DWARF by DEFAULT even at -O2, and nothing passed -g0, so a
# cross-compiled --emit=lib artifact was overwhelmingly debug information.
#
# Asserts, in rising order of what would actually break a user:
#   - --size is accepted and the binary still runs (native exe)
#   - --emit=lib under --size keeps its dynamic symbols (a stripped library
#     with no symbols links fine and is useless)
#   - --emit=obj under --size keeps its symbols (stripping an object would
#     remove what whoever links it next needs)
#   - the cross wasm artifact is dramatically smaller AND still valid
#
# The cross half is skipped without zig. Cost: ONE cross link (~90 TUs; no
# per-target archive cache), matching the convention in the neighbouring
# cross tests.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] size_mode: ae not built"
    exit 0
fi

TMPDIR_T="$(mktemp -d)"
cleanup() { rm -rf "$TMPDIR_T"; }
trap cleanup EXIT

LIB="$SCRIPT_DIR/sizelib.ae"

# ---- 1. a --size executable still runs -----------------------------------
cat > "$TMPDIR_T/app.ae" <<'AEEOF'
main() {
    println("size mode ok")
}
AEEOF
"$AE" build --size "$TMPDIR_T/app.ae" -o "$TMPDIR_T/app" >/dev/null 2>&1 \
    || { echo "  [FAIL] size_mode: --size build failed"; exit 1; }
OUT=$("$TMPDIR_T/app" 2>&1) || { echo "  [FAIL] size_mode: --size binary did not run"; exit 1; }
[ "$OUT" = "size mode ok" ] || {
    echo "  [FAIL] size_mode: wrong output: $OUT"; exit 1; }

# ---- 2. --emit=lib keeps its dynamic symbols -----------------------------
# --strip-all removes the symbol TABLE but must leave the dynamic symbols a
# consumer resolves against; a .so with neither would satisfy a size check
# and be unusable.
if "$AE" build --emit=lib --size "$LIB" -o "$TMPDIR_T/lib.so" >/dev/null 2>&1; then
    if command -v nm >/dev/null 2>&1; then
        if ! nm -D --defined-only "$TMPDIR_T/lib.so" 2>/dev/null | grep -q 'safe'; then
            echo "  [FAIL] size_mode: --emit=lib --size stripped the dynamic symbols"
            exit 1
        fi
    fi
fi

# ---- 3. --emit=obj keeps its symbols -------------------------------------
# An object file is linked later by someone else, so stripping it would
# remove exactly what they need. --size must not apply link-time stripping
# to a mode that does not link.
if "$AE" build --emit=obj --size "$LIB" -o "$TMPDIR_T/lib.o" >/dev/null 2>&1; then
    if command -v nm >/dev/null 2>&1; then
        if ! nm --defined-only "$TMPDIR_T/lib.o" 2>/dev/null | grep -q 'safe'; then
            echo "  [FAIL] size_mode: --emit=obj --size stripped the object's symbols"
            exit 1
        fi
    fi
fi

# ---- 4. the cross case, which is what this mode is for -------------------
if ! command -v zig >/dev/null 2>&1; then
    echo "  [PASS] size_mode: native checks (cross skipped: zig not on PATH)"
    exit 0
fi

BASE="$TMPDIR_T/base.wasm"
SIZED="$TMPDIR_T/sized.wasm"
"$AE" build --target=wasm32-wasi --emit=lib "$LIB" -o "$BASE" >/dev/null 2>&1 \
    || { echo "  [FAIL] size_mode: baseline wasm build failed"; exit 1; }
"$AE" build --target=wasm32-wasi --emit=lib --size "$LIB" -o "$SIZED" >/dev/null 2>&1 \
    || { echo "  [FAIL] size_mode: --size wasm build failed"; exit 1; }

BASE_SZ=$(wc -c < "$BASE" | tr -d '[:space:]')
SIZED_SZ=$(wc -c < "$SIZED" | tr -d '[:space:]')

# The measured ratio is ~38x; assert a conservative 4x so a partial
# regression (say, -g0 lost but stripping kept) still trips this.
if [ "$SIZED_SZ" -ge $((BASE_SZ / 4)) ]; then
    echo "  [FAIL] size_mode: --size wasm not meaningfully smaller"
    echo "         baseline=$BASE_SZ sized=$SIZED_SZ (wanted < baseline/4)"
    exit 1
fi

# Smaller is worthless if it is no longer a wasm module.
case "$(file -b "$SIZED" 2>/dev/null)" in
    *WebAssembly*) ;;
    *)
        echo "  [FAIL] size_mode: --size output is not a wasm module:"
        echo "         $(file -b "$SIZED" 2>/dev/null)"
        exit 1
        ;;
esac

# ...and worthless again if the exports it exists to provide are gone.
for sym in aether_risky aether_safe; do
    if ! strings "$SIZED" | grep -q "$sym"; then
        echo "  [FAIL] size_mode: $sym missing from the --size module"
        exit 1
    fi
done

echo "  [PASS] size_mode: wasm ${BASE_SZ} -> ${SIZED_SZ} bytes, exports and symbols intact"
