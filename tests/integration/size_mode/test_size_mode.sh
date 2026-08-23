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
# Keep the compiler's own message: a bad flag (say, one the host gcc is too
# old for) is invisible if this is swallowed, and "--size build failed" alone
# sends you looking in the wrong place.
if ! "$AE" build --size "$TMPDIR_T/app.ae" -o "$TMPDIR_T/app" \
        >"$TMPDIR_T/build.log" 2>&1; then
    echo "  [FAIL] size_mode: --size build failed"
    sed -n '1,15p' "$TMPDIR_T/build.log" | sed 's/^/         /'
    exit 1
fi
OUT=$("$TMPDIR_T/app" 2>&1) || { echo "  [FAIL] size_mode: --size binary did not run"; exit 1; }
[ "$OUT" = "size mode ok" ] || {
    echo "  [FAIL] size_mode: wrong output: $OUT"; exit 1; }

# ---- 2. --emit=lib keeps the symbols a consumer resolves against ---------
# Stripping removes the STATIC symbol table but must leave the dynamic /
# external ones; a library with neither would satisfy a size check and be
# unusable.
#
# The listing command is not portable. GNU nm spells it `nm -D`; BSD/macOS nm
# has no -D at all and spells defined-external `nm -gU`. Getting this wrong
# is not a harmless mismatch: the unsupported form exits non-zero with empty
# output, which reads exactly like "the symbols are gone" and fails the test
# on a perfectly good library. So pick by platform, and SKIP rather than fail
# if neither form works -- an inconclusive probe must not masquerade as a
# regression.
if "$AE" build --emit=lib --size "$LIB" -o "$TMPDIR_T/lib.so" >/dev/null 2>&1; then
    SYMS=""
    case "$(uname -s 2>/dev/null)" in
        Darwin) SYMS=$(nm -gU "$TMPDIR_T/lib.so" 2>/dev/null || true) ;;
        *)      SYMS=$(nm -D --defined-only "$TMPDIR_T/lib.so" 2>/dev/null || true) ;;
    esac
    if [ -n "$SYMS" ]; then
        if ! printf '%s' "$SYMS" | grep -q 'safe'; then
            echo "  [FAIL] size_mode: --emit=lib --size dropped the exported symbols"
            echo "         (listing was non-empty, so this is a real strip, not a"
            echo "          missing nm option)"
            exit 1
        fi
    fi
fi

# ---- 3. --emit=obj keeps its symbols -------------------------------------
# An object file is linked later by someone else, so stripping it would
# remove exactly what they need. --size must not apply link-time stripping
# to a mode that does not link.
# Plain `nm` (no -D) works on both toolchains for an object file, but the
# same "empty means inconclusive" rule applies.
if "$AE" build --emit=obj --size "$LIB" -o "$TMPDIR_T/lib.o" >/dev/null 2>&1; then
    OSYMS=$(nm "$TMPDIR_T/lib.o" 2>/dev/null || true)
    if [ -n "$OSYMS" ]; then
        if ! printf '%s' "$OSYMS" | grep -q 'safe'; then
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
