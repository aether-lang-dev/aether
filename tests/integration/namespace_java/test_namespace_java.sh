#!/bin/sh
# End-to-end Java SDK generator test. `ae build --namespace` emits both
# the .so and the Java SDK; the Check.java program imports the SDK
# class and exercises the full surface.
#
# Skips cleanly if javac/java aren't installed or the JDK is older than
# 22 (Panama Foreign Function & Memory API isn't stable before then).
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

if ! command -v javac >/dev/null 2>&1 || ! command -v java >/dev/null 2>&1; then
    echo "  [SKIP] test_namespace_java (javac/java not installed)"
    exit 0
fi

# Probe JDK version. Panama (java.lang.foreign.*) stabilized in JDK 22.
JVER=$(javac -version 2>&1 | sed -n 's/^javac \([0-9][0-9]*\).*/\1/p')
if [ -z "$JVER" ] || [ "$JVER" -lt 22 ]; then
    echo "  [SKIP] test_namespace_java (need JDK 22+, found '$JVER')"
    exit 0
fi

case "$(uname -s 2>/dev/null)" in
    Darwin) LIB_EXT=".dylib" ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT) LIB_EXT=".dll" ;;
    *)      LIB_EXT=".so" ;;
esac

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT
pass=0; fail=0

cp "$SCRIPT_DIR"/manifest.ae "$SCRIPT_DIR"/calc.ae "$SCRIPT_DIR"/Check.java "$TMPDIR/"

cd "$TMPDIR"
if ! AETHER_HOME="" "$ROOT/build/ae" build --namespace . >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] ae build --namespace ."; cat "$TMPDIR/build.log"; fail=$((fail + 1))
elif [ ! -f "$TMPDIR/com/example/calc/CalcGeneratedSdk.java" ]; then
    echo "  [FAIL] CalcGeneratedSdk.java was not generated"; find "$TMPDIR" -type f; fail=$((fail + 1))
else
    LIB_PATH=""
    for c in "$TMPDIR/lib"*"${LIB_EXT}"; do
        [ -f "$c" ] && { LIB_PATH="$c"; break; }
    done
    if [ -z "$LIB_PATH" ]; then
        echo "  [FAIL] no namespace lib"; fail=$((fail + 1))
    elif ! javac com/example/calc/CalcGeneratedSdk.java Check.java >"$TMPDIR/javac.log" 2>&1; then
        echo "  [FAIL] javac couldn't compile"; cat "$TMPDIR/javac.log"; fail=$((fail + 1))
    elif java --enable-native-access=ALL-UNNAMED Check "$LIB_PATH" >"$TMPDIR/run.out" 2>&1; then
        echo "  [PASS] generated Java SDK round-trips"; pass=$((pass + 1))
        if grep -qF "[ae] double_it" "$TMPDIR/run.out"; then
            echo "  [PASS] Aether [ae] script-side output visible to host"
            pass=$((pass + 1))
        else
            echo "  [FAIL] Aether [ae] script-side output missing from stdout"
            cat "$TMPDIR/run.out"
            fail=$((fail + 1))
        fi
    else
        echo "  [FAIL] Check.java failed"; cat "$TMPDIR/run.out"; fail=$((fail + 1))
    fi
fi

echo ""
echo "namespace_java: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
