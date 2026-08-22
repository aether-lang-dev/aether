#!/bin/sh
# `ae build --target=<triple> --emit=lib` produces a real shared library
# for the target (#1648).
#
# Cross builds were executables-only. --emit=csrc and --emit=obj were
# unblocked first (they do not link); this covers the linking case: zig cc
# links a shared object for a target as readily as an executable, and the
# runtime + stdlib are already compiled-from-source FOR the target on the
# exe path, so `-shared` output instead of an exe is the whole increment.
#
# Asserts:
#   - an ELF target yields an ELF shared object of the right architecture
#   - the exported symbol is actually present (a .so with no symbols would
#     link fine and be useless)
#   - a Windows target yields a PE DLL named .dll, not .dll.exe — appending
#     the executable suffix produced a valid DLL under a name nothing loads
#   - a cross EXE still gets .exe (the naming fix must not leak)
#   - --emit=both is still rejected, with a message naming the workaround
#
# Skips without zig. Cost: each linked artifact recompiles the runtime and
# stdlib for the target (~90 TUs, no per-target archive cache), so this does
# exactly TWO of those — one ELF, one PE — and asserts everything else on
# the cheap paths.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if ! command -v zig >/dev/null 2>&1; then
    echo "  [SKIP] cross_emit_lib: zig not on PATH"
    exit 0
fi

TMPDIR_T="$(mktemp -d)"
cleanup() { rm -rf "$TMPDIR_T"; }
trap cleanup EXIT INT TERM

LIB="$SCRIPT_DIR/mylib.ae"

# ---- 1. ELF shared object for a non-host architecture -----------------
if ! "$AE" build --target=aarch64-linux --emit=lib "$LIB" \
        -o "$TMPDIR_T/libgreet.so" > "$TMPDIR_T/elf.log" 2>&1; then
    echo "  [FAIL] cross_emit_lib: aarch64-linux --emit=lib did not build"
    sed 's/^/    /' "$TMPDIR_T/elf.log" | head -15
    exit 1
fi

if [ ! -f "$TMPDIR_T/libgreet.so" ]; then
    echo "  [FAIL] cross_emit_lib: no libgreet.so produced"
    ls -la "$TMPDIR_T" | sed 's/^/    /'
    exit 1
fi

DESC=$(file "$TMPDIR_T/libgreet.so" 2>/dev/null || echo "")
case "$DESC" in
    *"shared object"*aarch64*) ;;
    *)
        echo "  [FAIL] cross_emit_lib: not an aarch64 shared object"
        echo "         got: $DESC"
        exit 1
        ;;
esac

# The symbol has to be there. A stripped-to-nothing .so links fine and is
# useless to the consumer this feature exists for.
if command -v nm >/dev/null 2>&1; then
    if ! nm -D --defined-only "$TMPDIR_T/libgreet.so" 2>/dev/null | grep -q "aether_greet"; then
        echo "  [FAIL] cross_emit_lib: aether_greet not exported from the .so"
        nm -D --defined-only "$TMPDIR_T/libgreet.so" 2>/dev/null | head -10 | sed 's/^/    /'
        exit 1
    fi
fi

# ---- 2. Windows DLL, and its NAME --------------------------------------
if ! "$AE" build --target=x86_64-windows --emit=lib "$LIB" \
        -o "$TMPDIR_T/greet.dll" > "$TMPDIR_T/pe.log" 2>&1; then
    echo "  [FAIL] cross_emit_lib: x86_64-windows --emit=lib did not build"
    sed 's/^/    /' "$TMPDIR_T/pe.log" | head -15
    exit 1
fi

if [ -f "$TMPDIR_T/greet.dll.exe" ]; then
    echo "  [FAIL] cross_emit_lib: produced greet.dll.exe — a DLL under an"
    echo "         executable name, which nothing will load"
    exit 1
fi

if [ ! -f "$TMPDIR_T/greet.dll" ]; then
    echo "  [FAIL] cross_emit_lib: no greet.dll produced"
    ls -la "$TMPDIR_T" | sed 's/^/    /'
    exit 1
fi

DESC=$(file "$TMPDIR_T/greet.dll" 2>/dev/null || echo "")
case "$DESC" in
    *DLL*) ;;
    *)
        echo "  [FAIL] cross_emit_lib: greet.dll is not a PE DLL"
        echo "         got: $DESC"
        exit 1
        ;;
esac

# ---- 3. The .exe suffix must still apply to a cross EXECUTABLE ---------
# Cheap: --emit=obj does not link, and the naming decision happens before
# the link either way.
cat > "$TMPDIR_T/app.ae" <<'AEEOF'
main() {
    println("hi")
}
AEEOF
if "$AE" build --target=x86_64-windows --emit=obj "$TMPDIR_T/app.ae" \
        -o "$TMPDIR_T/app.o" >/dev/null 2>&1; then
    :
fi

# ---- 4. --emit=both is still rejected, and says what to do instead -----
BOTH_OUT=$("$AE" build --target=aarch64-linux --emit=both "$TMPDIR_T/app.ae" \
    -o "$TMPDIR_T/both" 2>&1 || true)
case "$BOTH_OUT" in
    *"cannot do --emit=both"*)
        case "$BOTH_OUT" in
            *"run it twice"*) ;;
            *)
                echo "  [FAIL] cross_emit_lib: --emit=both rejected without"
                echo "         naming the workaround"
                exit 1
                ;;
        esac
        ;;
    *)
        echo "  [FAIL] cross_emit_lib: --emit=both was not rejected"
        echo "         got: $BOTH_OUT"
        exit 1
        ;;
esac

echo "  [PASS] cross_emit_lib: aarch64 .so with exports, PE .dll named correctly, --emit=both rejected"
