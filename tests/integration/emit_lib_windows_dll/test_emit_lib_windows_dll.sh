#!/bin/sh
# A native Windows `ae build --emit=lib` links a DLL that a C host can load.
#
# The -shared / --export-all-symbols flags for a Windows library lived only
# in the POSIX branch of build_gcc_cmd, under an `#ifdef _WIN32` that can
# never be true there (#993's flag was dead code), so a native
# `ae build --emit=lib` on Windows linked an EXECUTABLE from main-less lib
# codegen and failed with "undefined reference to WinMain". The output was
# already named lib<name>.dll. Windows-only: the POSIX library path has its
# own tests (lib_meta, manifest, notify, ...), which are skipped here.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) ;;
    *) echo "  [SKIP] emit_lib_windows_dll: Windows DLL link path only"; exit 0 ;;
esac
if [ ! -x "$AE" ]; then
    echo "  [SKIP] emit_lib_windows_dll: $AE not built"
    exit 0
fi
command -v gcc >/dev/null 2>&1 || { echo "  [SKIP] emit_lib_windows_dll: gcc not on PATH"; exit 0; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0
export AETHER_HOME="$ROOT"

cat > "$tmp/mathlib.ae" <<'AE'
add_them(a: int, b: int) -> int { return a + b + 40 }
AE

# 1. the build links a DLL (a dev-tree `ae` puts an unnamed output under
#    <root>/build, so name it)
if ! (cd "$tmp" && "$AE" build --emit=lib mathlib.ae -o "$tmp/libmathlib.dll" > "$tmp/build.log" 2>&1); then
    echo "  [FAIL] emit_lib_windows_dll: ae build --emit=lib failed"
    grep -i "undefined\|error" "$tmp/build.log" | head -3 | sed 's/^/        /'
    exit 1
fi
dll="$(find "$tmp" -name 'libmathlib.dll' | head -1)"
if [ -z "$dll" ]; then
    echo "  [FAIL] emit_lib_windows_dll: no libmathlib.dll produced"
    ls "$tmp" | sed 's/^/        /'
    exit 1
fi

# 2. it is a DLL (PE with the DLL characteristic), and exports the catalog symbol
if command -v objdump >/dev/null 2>&1; then
    if ! objdump -p "$dll" 2>/dev/null | grep -q "aether_add_them"; then
        echo "  [FAIL] emit_lib_windows_dll: aether_add_them is not exported from the DLL"
        fail=1
    fi
    if ! objdump -p "$dll" 2>/dev/null | grep -qi "DLL"; then
        echo "  [FAIL] emit_lib_windows_dll: the artifact is not a DLL"
        fail=1
    fi
fi

# 3. a C host loads it and calls through GetProcAddress
cat > "$tmp/host.c" <<'C'
#include <windows.h>
#include <stdio.h>
int main(int argc, char** argv) {
    HMODULE h = LoadLibraryA(argv[1]);
    if (!h) { printf("LoadLibrary failed: %lu\n", (unsigned long)GetLastError()); return 2; }
    typedef int (*fn_t)(int, int);
    fn_t f = (fn_t)GetProcAddress(h, "aether_add_them");
    if (!f) { printf("GetProcAddress failed\n"); return 3; }
    printf("%d\n", f(1, 1));
    return 0;
}
C
if ! gcc "$tmp/host.c" -o "$tmp/host.exe" > "$tmp/host.log" 2>&1; then
    echo "  [FAIL] emit_lib_windows_dll: host compile failed"
    head -5 "$tmp/host.log" | sed 's/^/        /'
    exit 1
fi
got="$("$tmp/host.exe" "$dll" 2>&1)"
if [ "$got" != "42" ]; then
    echo "  [FAIL] emit_lib_windows_dll: host call through the DLL (got '$got', want 42)"
    fail=1
fi

# 4. `ae build --namespace .` on Windows: the library is named from the
#    manifest (libgreet.dll) and exports aether_describe. The name came
#    from a popen() of aetherc that cmd.exe mangled (a command line that
#    starts with a quote loses its first and last quote), and the
#    directory-basename fallback scanned for '/' only, so the output was
#    "./libD:\...\dir.c" — "Error opening output file: Invalid argument".
mkdir -p "$tmp/ns"
cp "$ROOT/tests/integration/namespace_basic/manifest.ae" "$ROOT/tests/integration/namespace_basic/hello.ae" "$tmp/ns/"
if ! (cd "$tmp/ns" && AETHER_HOME="$ROOT" "$AE" build --namespace . > "$tmp/ns.log" 2>&1); then
    echo "  [FAIL] emit_lib_windows_dll: ae build --namespace . failed"
    grep -iv "^\s*$" "$tmp/ns.log" | head -4 | sed 's/^/        /'
    fail=1
elif [ ! -f "$tmp/ns/libgreet.dll" ]; then
    echo "  [FAIL] emit_lib_windows_dll: --namespace did not produce libgreet.dll (named from the manifest)"
    ls "$tmp/ns" | sed 's/^/        /'
    fail=1
elif command -v objdump >/dev/null 2>&1 && ! objdump -p "$tmp/ns/libgreet.dll" | grep -q "aether_describe"; then
    echo "  [FAIL] emit_lib_windows_dll: libgreet.dll does not export aether_describe"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] emit_lib_windows_dll: ae build --emit=lib links libmathlib.dll, exports aether_add_them, a C host loads and calls it; --namespace builds libgreet.dll"
fi
exit $fail
