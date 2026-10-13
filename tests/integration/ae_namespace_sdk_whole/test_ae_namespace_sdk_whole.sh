#!/bin/sh
# Regression (#2888): `ae build --namespace` binds every function of the
# namespace, with every parameter, under its whole name.
#
# The SDK generator read aetherc's function list into 16 KB and parsed it
# into a fixed table: 64 functions, 16 parameters each, a name of 127 bytes
# and a type of 63. Past those it dropped functions and parameters without
# a word, and a longer name was cut (since #2862, refused, so no SDK was
# written at all). The list is now read whole and grown, every name and
# type kept as it is, and the Java method name (lowerCamel) is built in a
# buffer its name's size. The namespace below has 70 functions, one with
# 20 parameters and one whose name is 148 bytes long; the generated Python
# and Java sources must name all of them. Only the sources are checked, so
# neither Python nor a JDK is needed.
#
# The manifest's path is built whole too: on Linux, a namespace directory
# longer than 1024 bytes without a manifest.ae is reported under its whole
# path (it was cut, and the cut path reported missing).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
EXE=""
[ -x "$ROOT/build/ae" ] || EXE=".exe"
AE="$ROOT/build/ae$EXE"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] ae_namespace_sdk_whole: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0
ns="$TMP/ns"
mkdir -p "$ns"

cat > "$ns/manifest.ae" <<'AE'
import std.host

abi() {
    describe("wide") {
        input("limit", "int")
        event("Done", "int64")
        bindings() {
            python("wide_sdk")
            java("com.example.wide", "WideSdk")
        }
    }
}
AE

long_name="long_$(printf 'abcdefghi_%.0s' $(seq 1 14))end"
{
    i=0
    while [ $i -lt 70 ]; do
        printf 'f%d(n: int) -> int {\n    return n + %d\n}\n\n' "$i" "$i"
        i=$((i + 1))
    done
    printf 'sum20('
    i=0
    while [ $i -lt 20 ]; do
        [ $i -gt 0 ] && printf ', '
        printf 'p%d: int' "$i"
        i=$((i + 1))
    done
    printf ') -> int {\n    return p0 + p19\n}\n\n'
    printf '%s(n: int) -> int {\n    return n\n}\n' "$long_name"
} > "$ns/funcs.ae"

if ! ( cd "$ns" && AETHER_HOME="" "$AE" build --namespace . ) > "$TMP/build.log" 2>&1; then
    echo "  [FAIL] ae_namespace_sdk_whole: ae build --namespace"
    tail -15 "$TMP/build.log" | sed 's/^/        /'
    exit 1
fi

py="$ns/wide_sdk.py"
java="$(find "$ns" -name 'WideSdk.java' | head -1)"
if [ ! -f "$py" ] || [ -z "$java" ]; then
    echo "  [FAIL] ae_namespace_sdk_whole: the Python or Java SDK was not written"
    tail -15 "$TMP/build.log" | sed 's/^/        /'
    exit 1
fi

# 70 functions, each bound in both.
missing=""
i=0
while [ $i -lt 70 ]; do
    grep -q "def f$i(self" "$py" || missing="$missing py:f$i"
    grep -q "public int f$i(" "$java" || missing="$missing java:f$i"
    i=$((i + 1))
done
if [ -n "$missing" ]; then
    echo "  [FAIL] ae_namespace_sdk_whole: functions not bound:$missing"
    fail=1
fi

# All 20 parameters, in order.
params="self"
jparams=""
i=0
while [ $i -lt 20 ]; do
    params="$params, p$i"
    [ $i -gt 0 ] && jparams="$jparams, "
    jparams="${jparams}int p$i"
    i=$((i + 1))
done
if ! grep -qF "def sum20($params):" "$py" || ! grep -qF "public int sum20($jparams)" "$java"; then
    echo "  [FAIL] ae_namespace_sdk_whole: sum20 is not bound with its 20 parameters"
    grep "sum20" "$py" "$java" | head -4 | cut -c1-200 | sed 's/^/        /'
    fail=1
fi

# The 148-byte name, whole in Python and as lowerCamel in Java.
camel="$(printf '%s' "$long_name" | awk -F_ '{ s = $1; for (i = 2; i <= NF; i++) s = s toupper(substr($i, 1, 1)) substr($i, 2); print s }')"
if ! grep -qF "def $long_name(self, n):" "$py" || ! grep -qF "public int $camel(int n)" "$java"; then
    echo "  [FAIL] ae_namespace_sdk_whole: the ${#long_name}-byte name is not bound whole"
    fail=1
fi

# A namespace directory past 1024 bytes, without a manifest: reported
# under its whole path. Linux, where such a path exists for ae to find.
if [ "$(uname -s 2>/dev/null)" = "Linux" ]; then
    deep="$TMP/deep"
    seg="ddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
    while [ ${#deep} -lt 1100 ]; do deep="$deep/$seg"; done
    if mkdir -p "$deep" 2>/dev/null; then
        out="$(cd "$TMP" && "$AE" build --namespace "$deep" 2>&1)"
        if ! printf '%s' "$out" | grep -qF "$deep/manifest.ae not found"; then
            echo "  [FAIL] ae_namespace_sdk_whole: a ${#deep}-byte namespace directory's manifest path was not reported whole"
            printf '%s\n' "$out" | head -3 | cut -c1-200 | sed 's/^/        /'
            fail=1
        fi
    fi
fi

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "  [PASS] ae_namespace_sdk_whole: 70 functions, 20 parameters and a ${#long_name}-byte name are bound whole"
exit 0
