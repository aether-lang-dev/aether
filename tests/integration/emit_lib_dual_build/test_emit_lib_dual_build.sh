#!/bin/sh
# Verifies:
#   (a) `ae build --emit=both` succeeds, producing BOTH a runnable
#       executable AND a shared library from one source invocation
#   (b) The same source can also still be built twice independently
#       — once as exe, once as lib — and both artifacts work

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s 2>/dev/null)" in
    Darwin) LIB_EXT=".dylib" ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT) LIB_EXT=".dll" ;;
    *)      LIB_EXT=".so" ;;
esac

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT
pass=0; fail=0

# A library's exported symbols, one " T <name>" per line: a DLL's export
# table (objdump -p) on Windows, else nm -g.
lib_syms() {
    case "$1" in
        *.dll) objdump -p "$1" 2>/dev/null \
                   | sed -n '/\[Ordinal\/Name Pointer\] Table/,/^$/s/.* \([^ ]*\)$/ T \1/p' ;;
        *)     nm -g "$1" 2>/dev/null ;;
    esac
}

# (a) --emit=both should produce both artifacts in a single invocation.
cd "$SCRIPT_DIR"
if ! AETHER_HOME="" "$ROOT/build/ae" build --emit=both config.ae -o "$TMPDIR/both_out" >"$TMPDIR/both.log" 2>&1; then
    echo "  [FAIL] --emit=both build failed"
    cat "$TMPDIR/both.log"
    fail=$((fail + 1))
else
    # The exe lands at the -o path verbatim (no extension on POSIX).
    BOTH_EXE="$TMPDIR/both_out"
    # The lib lands at lib<base><ext> in the same directory the -o
    # points at — the lib-mode default-naming logic in ae build.
    BOTH_LIB=""
    for c in "$TMPDIR/libboth_out${LIB_EXT}" "$TMPDIR/both_out${LIB_EXT}" "$TMPDIR/both_out"; do
        [ -f "$c" ] && [ "$c" != "$BOTH_EXE" ] && { BOTH_LIB="$c"; break; }
    done
    if [ ! -x "$BOTH_EXE" ]; then
        echo "  [FAIL] --emit=both produced no executable at $BOTH_EXE"
        ls -la "$TMPDIR" | head -10
        fail=$((fail + 1))
    elif out=$("$BOTH_EXE" 2>/dev/null) && echo "$out" | grep -q "ran as exe"; then
        echo "  [PASS] --emit=both produced runnable exe"
        pass=$((pass + 1))
    else
        echo "  [FAIL] --emit=both exe didn't print 'ran as exe'"
        echo "       got: $out"
        fail=$((fail + 1))
    fi
    if [ -z "$BOTH_LIB" ] || [ ! -f "$BOTH_LIB" ]; then
        echo "  [FAIL] --emit=both produced no shared library"
        ls -la "$TMPDIR" | head -10
        fail=$((fail + 1))
    elif lib_syms "$BOTH_LIB" | grep -qE " T _?aether_greet$"; then
        echo "  [PASS] --emit=both lib exports aether_greet"
        pass=$((pass + 1))
    else
        echo "  [FAIL] --emit=both lib missing aether_greet symbol"
        lib_syms "$BOTH_LIB" | head -20
        fail=$((fail + 1))
    fi
fi

# (b1) Build as exe — should run and print "ran as exe".
if ! AETHER_HOME="" "$ROOT/build/ae" build --emit=exe config.ae -o "$TMPDIR/dual_exe" >"$TMPDIR/exe.log" 2>&1; then
    echo "  [FAIL] --emit=exe build failed"
    cat "$TMPDIR/exe.log"
    fail=$((fail + 1))
elif out=$("$TMPDIR/dual_exe" 2>/dev/null); then
    if echo "$out" | grep -q "ran as exe"; then
        echo "  [PASS] exe artifact runs main()"
        pass=$((pass + 1))
    else
        echo "  [FAIL] exe ran but stdout didn't include 'ran as exe'"
        echo "       got: $out"
        fail=$((fail + 1))
    fi
else
    echo "  [FAIL] exe artifact failed to execute"
    fail=$((fail + 1))
fi

# (b2) Build same source as lib — should have aether_greet symbol, no main.
if ! AETHER_HOME="" "$ROOT/build/ae" build --emit=lib config.ae -o "$TMPDIR/libdual" >"$TMPDIR/lib.log" 2>&1; then
    echo "  [FAIL] --emit=lib build failed"
    cat "$TMPDIR/lib.log"
    fail=$((fail + 1))
else
    LIB_PATH=""
    for c in "$TMPDIR/libdual" "$TMPDIR/libdual${LIB_EXT}"; do
        [ -f "$c" ] && { LIB_PATH="$c"; break; }
    done
    if [ -z "$LIB_PATH" ]; then
        echo "  [FAIL] no lib produced"; fail=$((fail + 1))
    else
        # aether_greet present? (macOS nm prefixes symbols with `_`, Linux
        # nm does not. `nm -g` is the portable "external symbols only" flag;
        # grep matches either prefix form. A DLL's are its export table.)
        if lib_syms "$LIB_PATH" | grep -qE " T _?aether_greet$"; then
            echo "  [PASS] lib artifact exports aether_greet"
            pass=$((pass + 1))
        else
            echo "  [FAIL] aether_greet symbol missing from lib"
            lib_syms "$LIB_PATH" | head -20
            fail=$((fail + 1))
        fi
        # main absent?
        if lib_syms "$LIB_PATH" | grep -qE " T _?main$"; then
            echo "  [FAIL] lib artifact has 'main' symbol — should be suppressed"
            fail=$((fail + 1))
        else
            echo "  [PASS] lib artifact has no 'main' symbol"
            pass=$((pass + 1))
        fi
    fi
fi

echo ""
echo "emit_lib_dual_build: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
