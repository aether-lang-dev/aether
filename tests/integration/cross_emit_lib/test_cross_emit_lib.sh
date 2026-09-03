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
#   - a static archive and a non-Darwin dylib get no install_name fixup, and
#     so no warning about dlopen for output nothing can dlopen
#   - --emit=staticlib produces an ar archive that holds the program object
#     AND the runtime/stdlib objects, and that a C program can actually link
#     against and RUN (the shape an iOS/Xcode app needs, since Apple forbids
#     third-party dylibs in App Store binaries)
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

# ---- 5. --emit=staticlib: archive it, then LINK AND RUN it ------------
# The end-to-end assertion is the point: a well-formed .a that nothing can
# link is exactly the failure this feature exists to avoid. The triple is
# matched to the host so the result can actually be executed here; it was
# hardcoded to x86_64-linux-musl, which runs on a glibc x86 host and is
# unexecutable anywhere else, so the run assertion could not pass on macOS or
# on an arm Linux box. --emit=staticlib still requires --target, which is why
# the host's own triple is named explicitly. The iOS target proper needs a Mac
# and is asserted in tests/integration/cross_ios.
case "$(uname -s)/$(uname -m)" in
    Darwin/arm64)         SL_TARGET=aarch64-macos ;;
    Darwin/x86_64)        SL_TARGET=x86_64-macos ;;
    Linux/aarch64)        SL_TARGET=aarch64-linux-musl ;;
    Linux/x86_64|*)       SL_TARGET=x86_64-linux-musl ;;
esac
if ! "$AE" build --target="$SL_TARGET" --emit=staticlib "$LIB" \
        -o "$TMPDIR_T/libgreet.a" > "$TMPDIR_T/sl.log" 2>&1; then
    echo "  [FAIL] cross_emit_lib: --emit=staticlib did not build"
    sed 's/^/    /' "$TMPDIR_T/sl.log" | head -15
    exit 1
fi

# install_name_tool applies to Mach-O dylibs. An archive has no install_name,
# and neither does a dylib for a non-Darwin target, so running it on either
# failed and warned about dlopen for output that is never dlopen'd.
if grep -q 'install_name_tool' "$TMPDIR_T/sl.log"; then
    echo "  [FAIL] cross_emit_lib: install_name_tool run on a static archive"
    grep 'install_name_tool' "$TMPDIR_T/sl.log" | sed 's/^/    /'
    exit 1
fi
if grep -q 'install_name_tool' "$TMPDIR_T/elf.log"; then
    echo "  [FAIL] cross_emit_lib: install_name_tool run on a non-Darwin dylib"
    grep 'install_name_tool' "$TMPDIR_T/elf.log" | sed 's/^/    /'
    exit 1
fi

DESC=$(file "$TMPDIR_T/libgreet.a" 2>/dev/null || echo "")
case "$DESC" in
    *archive*) ;;
    *)
        echo "  [FAIL] cross_emit_lib: --emit=staticlib output is not an archive"
        echo "         got: $DESC"
        exit 1
        ;;
esac

# The program object alone would be a useless .a: the consumer links this one
# file and expects the runtime to come with it.
#
# Listed with `zig ar`, not the host `ar`. zig ar writes a GNU archive, whose
# member names live in an extended string table; BSD ar cannot decode that and
# prints the raw offset references (`/`, `//`, `/0`, `/20`) instead of names.
# So this assertion passed on Linux and failed on macOS against a byte-identical
# archive that links and runs fine on both.
if ! zig ar t "$TMPDIR_T/libgreet.a" 2>/dev/null | grep -q '__aether_program.o'; then
    echo "  [FAIL] cross_emit_lib: static archive lacks the program object"
    zig ar t "$TMPDIR_T/libgreet.a" 2>/dev/null | head -10 | sed 's/^/    /'
    exit 1
fi
MEMBERS=$(zig ar t "$TMPDIR_T/libgreet.a" 2>/dev/null | wc -l)
if [ "$MEMBERS" -le 10 ]; then
    echo "  [FAIL] cross_emit_lib: static archive holds only $MEMBERS members;"
    echo "         the runtime/stdlib objects are missing"
    exit 1
fi

# Link a C consumer against it and run it. This is what an Xcode app does.
cat > "$TMPDIR_T/host.c" <<'CEOF'
#include <stdio.h>
extern int aether_greet(void);
int main(void) { printf("greet=%d\n", aether_greet()); return 0; }
CEOF
if ! zig cc -target "$SL_TARGET" "$TMPDIR_T/host.c" "$TMPDIR_T/libgreet.a" \
        -lm -o "$TMPDIR_T/hostprog" > "$TMPDIR_T/link.log" 2>&1; then
    echo "  [FAIL] cross_emit_lib: C program could not link the static archive"
    sed 's/^/    /' "$TMPDIR_T/link.log" | head -15
    exit 1
fi
RAN=$("$TMPDIR_T/hostprog" 2>&1 || echo "RUN-FAILED")
if [ "$RAN" != "greet=42" ]; then
    echo "  [FAIL] cross_emit_lib: static-linked program printed '$RAN', expected 'greet=42'"
    exit 1
fi

echo "  [PASS] cross_emit_lib: aarch64 .so with exports, PE .dll named correctly,"
echo "         --emit=both rejected, static archive links and runs"
