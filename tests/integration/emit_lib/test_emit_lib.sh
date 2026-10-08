#!/bin/sh
# Test: --emit=lib produces a .so/.dylib/.dll with aether_<name> symbols
# that a C host can load (dlopen, or LoadLibrary on Windows) and call with
# the correct ABI.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

pass=0
fail=0

# Platform: Linux uses .so, macOS .dylib, Windows .dll (which needs no -ldl)
LDL="-ldl"
case "$(uname -s 2>/dev/null)" in
    Darwin) LIB_EXT=".dylib" ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT) LIB_EXT=".dll"; LDL="" ;;
    *)      LIB_EXT=".so" ;;
esac

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

# Step 1: compile config.ae with --emit=lib
cd "$SCRIPT_DIR"
if ! AETHER_HOME="" "$ROOT/build/ae" build --emit=lib config.ae -o "$TMPDIR/libconfig" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] ae build --emit=lib produced an error"
    cat "$TMPDIR/build.log"
    fail=$((fail + 1))
else
    # The ae build override renames <name> to lib<name><ext> only when
    # -o is not supplied. Since we passed -o, the output is literally
    # "libconfig" with no extension. We don't care about the filename
    # for this test; just find whatever was produced.
    LIB_PATH=""
    for candidate in "$TMPDIR/libconfig" "$TMPDIR/libconfig${LIB_EXT}"; do
        if [ -f "$candidate" ]; then
            LIB_PATH="$candidate"
            break
        fi
    done
    if [ -z "$LIB_PATH" ]; then
        echo "  [FAIL] ae build --emit=lib did not produce a library file"
        ls -la "$TMPDIR"
        fail=$((fail + 1))
    else
        # Step 2: compile the C consumer
        # shellcheck disable=SC2086
        if ! gcc "$SCRIPT_DIR/consume.c" $LDL -o "$TMPDIR/consume" 2>"$TMPDIR/gcc.log"; then
            echo "  [FAIL] gcc could not compile consume.c"
            cat "$TMPDIR/gcc.log"
            fail=$((fail + 1))
        else
            # Step 3: run the consumer against the library
            if "$TMPDIR/consume" "$LIB_PATH" >"$TMPDIR/run.out" 2>&1; then
                echo "  [PASS] aether_* symbols callable through the loaded library"
                pass=$((pass + 1))
            else
                echo "  [FAIL] consume reported an error"
                cat "$TMPDIR/run.out"
                fail=$((fail + 1))
            fi
        fi
    fi
fi

# Step 4: verify --emit=lib rejects capability-heavy imports
cat > "$TMPDIR/bad.ae" <<'EOF'
import std.net
sum(a: int, b: int) { return a + b }
EOF
if AETHER_HOME="" "$ROOT/build/aetherc" --emit=lib "$TMPDIR/bad.ae" "$TMPDIR/bad.c" >"$TMPDIR/bad.log" 2>&1; then
    echo "  [FAIL] --emit=lib did not reject 'import std.net'"
    fail=$((fail + 1))
else
    if grep -q "capability-empty" "$TMPDIR/bad.log"; then
        echo "  [PASS] --emit=lib rejects capability-heavy imports"
        pass=$((pass + 1))
    else
        echo "  [FAIL] --emit=lib rejected std.net but without the expected message"
        cat "$TMPDIR/bad.log"
        fail=$((fail + 1))
    fi
fi

echo ""
echo "emit_lib: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
