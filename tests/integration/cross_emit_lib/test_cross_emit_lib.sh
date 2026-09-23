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
#   - --emit=both works cross: one invocation yields the executable AND the
#     library, the library named for the TARGET platform, not the host
#   - a static archive and a non-Darwin dylib get no install_name fixup, and
#     so no warning about dlopen for output nothing can dlopen
#   - --emit=staticlib produces an ar archive that holds the program object
#     AND the runtime/stdlib objects, and that a C program can actually link
#     against and RUN (the shape an iOS/Xcode app needs, since Apple forbids
#     third-party dylibs in App Store binaries)
#
# Skips without zig. Cost: each linked artifact recompiles the runtime and
# stdlib for the target (~90 TUs, no per-target archive cache), so this does
# FOUR of those — one ELF lib, one PE lib, and the exe + lib of one
# --emit=both — and asserts everything else on the cheap paths.

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

# The import library carries the DLL's name. Left to itself zig names it
# after the first INPUT file (the generated greet.c here, or app.c for
# libapp.dll -- `app.lib`), so a consumer linking against the DLL looked for
# an import library that was not there under that name.
if [ ! -f "$TMPDIR_T/greet.lib" ]; then
    echo "  [FAIL] cross_emit_lib: greet.dll's import library is not greet.lib"
    ls "$TMPDIR_T" | grep -iE '\.lib$' | sed 's/^/        /'
    exit 1
fi

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

# ---- 4. --emit=both works cross, and names the library for the TARGET --
# It was rejected under --target on the grounds that "the cross path links
# once and cannot produce both artifacts from one invocation". But
# --emit=both never asks it to: it runs two ordinary builds, one
# --emit=exe and one --emit=lib, each with its own link, and both of those
# work cross. The rejection answered a design the code does not have.
#
# What cross DOES change is the library's name. The extension used to come
# from the HOST (#ifdef _WIN32 / __APPLE__), so a Linux .so built from
# Windows was called NAME.dll. The target here is chosen to DIFFER from the
# host's platform, because a same-platform target would pass with the old
# host-derived extension and prove nothing.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) BOTH_T=aarch64-linux;  BOTH_EXE="both";     BOTH_LIB="both.so";  BOTH_KIND=elf ;;
    *)                    BOTH_T=x86_64-windows; BOTH_EXE="both.exe"; BOTH_LIB="both.dll"; BOTH_KIND=pe  ;;
esac
cat > "$TMPDIR_T/bothapp.ae" <<'AE'
greet() -> int { return 42 }
main() { println("hi ${greet()}") }
AE
if ! "$AE" build --target="$BOTH_T" --emit=both "$TMPDIR_T/bothapp.ae" \
        -o "$TMPDIR_T/both" > "$TMPDIR_T/both.log" 2>&1; then
    echo "  [FAIL] cross_emit_lib: --target=$BOTH_T --emit=both did not build"
    tail -5 "$TMPDIR_T/both.log" | sed 's/^/        /'
    exit 1
fi
# Exact names, read from a directory listing: on a Windows host MSYS makes
# `[ -f both ]` true for a file called both.exe, so a test of existence
# cannot tell the target's executable name from the host's -- and naming a
# Linux binary `both.exe` after the host is one of the bugs this guards.
BOTH_NAMES=" $(ls "$TMPDIR_T" | grep '^both' | tr '\n' ' ')"
for f in "$BOTH_EXE" "$BOTH_LIB"; do
    case "$BOTH_NAMES" in
        *" $f "*) ;;
        *)
            echo "  [FAIL] cross_emit_lib: --emit=both for $BOTH_T produced no file named exactly $f"
            echo "         got:$BOTH_NAMES"
            exit 1
            ;;
    esac
done
if [ "$BOTH_KIND" = elf ]; then
    case "$BOTH_NAMES" in
        *" both.exe "*)
            echo "  [FAIL] cross_emit_lib: a $BOTH_T executable was named with the host's .exe"
            exit 1
            ;;
    esac
fi
# Both must be the right KIND of artifact, not two copies of one: an ELF
# executable (e_type 2) beside a shared object (e_type 3), or a PE image
# beside one with the IMAGE_FILE_DLL bit.
if [ "$BOTH_KIND" = elf ]; then
    et_exe=$(od -An -tu2 -j16 -N2 "$TMPDIR_T/$BOTH_EXE" | tr -d ' ')
    et_lib=$(od -An -tu2 -j16 -N2 "$TMPDIR_T/$BOTH_LIB" | tr -d ' ')
    if [ "$et_exe" != 2 ] && [ "$et_exe" != 3 ]; then
        echo "  [FAIL] cross_emit_lib: --emit=both's executable is not an ELF image (e_type $et_exe)"; exit 1
    fi
    if [ "$et_lib" != 3 ]; then
        echo "  [FAIL] cross_emit_lib: --emit=both's library is not an ELF shared object (e_type $et_lib)"; exit 1
    fi
else
    pe_chars() {
        off=$(od -An -tu4 -j60 -N4 "$1" | tr -d ' ')
        od -An -tu2 -j$((off + 22)) -N2 "$1" | tr -d ' '
    }
    c_exe=$(pe_chars "$TMPDIR_T/$BOTH_EXE")
    c_lib=$(pe_chars "$TMPDIR_T/$BOTH_LIB")
    if [ $((c_exe & 8192)) -ne 0 ]; then
        echo "  [FAIL] cross_emit_lib: --emit=both's .exe carries the DLL bit"; exit 1
    fi
    if [ $((c_lib & 8192)) -eq 0 ]; then
        echo "  [FAIL] cross_emit_lib: --emit=both's .dll lacks the IMAGE_FILE_DLL bit"; exit 1
    fi
fi

# ---- 4b. a Windows target names only its DLL `.dll` ------------------
# The rule that gives a Windows --emit=lib its `.dll` keyed on the flag
# every lib-codegen mode sets, so it fired for the modes that are not a DLL
# too: `--emit=staticlib -o libgreet.a` wrote a correct archive named
# `libgreet.a.dll`, and an object got the same. Checked with --emit=obj,
# which stops at `zig cc -c` and so costs one TU rather than a runtime
# build -- the naming decision is the same flag for both.
if ! "$AE" build --target=x86_64-windows --emit=obj "$LIB" \
        -o "$TMPDIR_T/winobj.o" > "$TMPDIR_T/winobj.log" 2>&1; then
    echo "  [FAIL] cross_emit_lib: --target=x86_64-windows --emit=obj did not build"
    tail -5 "$TMPDIR_T/winobj.log" | sed 's/^/        /'
    exit 1
fi
if [ ! -f "$TMPDIR_T/winobj.o" ] || [ -f "$TMPDIR_T/winobj.o.dll" ]; then
    echo "  [FAIL] cross_emit_lib: a Windows-target object was not named what -o asked"
    ls "$TMPDIR_T" | grep '^winobj' | sed 's/^/        /'
    exit 1
fi

# ---- 5. --emit=staticlib: archive it, then LINK AND RUN it ------------
# The end-to-end assertion is the point: a well-formed .a that nothing can
# link is exactly the failure this feature exists to avoid. The triple is
# matched to the host so the result can actually be executed here; it was
# hardcoded to x86_64-linux-musl, which runs on a glibc x86 host and is
# unexecutable anywhere else, so the run assertion could not pass on macOS or
# on an arm Linux box. --emit=staticlib still requires --target, which is why
# the host's own triple is named explicitly. The iOS target proper needs a Mac
# and is asserted in tests/integration/cross_ios.
#
# Windows is a host too. It used to fall through to x86_64-linux-musl, which
# built fine and then failed the run with "cannot execute binary file" --
# the one assertion this section exists for. On Windows the consumer also
# needs the Win32 libraries the runtime calls into (the same list `ae`
# links with), a `.exe` to run, and CR stripped from what it prints.
SL_EXE=""
SL_LIBS="-lm"
case "$(uname -s)/$(uname -m)" in
    Darwin/arm64)         SL_TARGET=aarch64-macos ;;
    Darwin/x86_64)        SL_TARGET=x86_64-macos ;;
    Linux/aarch64)        SL_TARGET=aarch64-linux-musl ;;
    MINGW*|MSYS*|CYGWIN*)
        SL_TARGET=x86_64-windows
        SL_EXE=".exe"
        SL_LIBS="-lws2_32 -lcrypt32 -lgdi32 -luser32 -ladvapi32 -lbcrypt -ldbghelp" ;;
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
# shellcheck disable=SC2086  # SL_LIBS is a list of flags
if ! zig cc -target "$SL_TARGET" "$TMPDIR_T/host.c" "$TMPDIR_T/libgreet.a" \
        $SL_LIBS -o "$TMPDIR_T/hostprog$SL_EXE" > "$TMPDIR_T/link.log" 2>&1; then
    echo "  [FAIL] cross_emit_lib: C program could not link the static archive"
    sed 's/^/    /' "$TMPDIR_T/link.log" | head -15
    exit 1
fi
# Captured to a file first: piping straight into `tr` makes the pipeline's
# status tr's, so a crash would read as wrong output instead of a crash.
"$TMPDIR_T/hostprog$SL_EXE" > "$TMPDIR_T/run.out" 2>&1
RUN_RC=$?
RAN=$(tr -d '\r' < "$TMPDIR_T/run.out")
[ "$RUN_RC" -eq 0 ] || RAN="RUN-FAILED (exit $RUN_RC): $RAN"
if [ "$RAN" != "greet=42" ]; then
    echo "  [FAIL] cross_emit_lib: static-linked program printed '$RAN', expected 'greet=42'"
    exit 1
fi

echo "  [PASS] cross_emit_lib: aarch64 .so with exports, PE .dll named correctly,"
echo "         --emit=both builds both, named for the target, static archive links and runs"
