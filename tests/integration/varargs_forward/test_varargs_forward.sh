#!/bin/sh
# Integration test for forwarding a variadic tail to C (#1244).
#
# Regression for a silent wrong-output bug: `va_start()` yields a cookie that
# points AT the function's va_list, and va_arg / va_end dereference it, but the
# call boundary did not. Passing the cookie to vsnprintf handed it a pointer
# where a va_list was expected, so the callee read the wrong bytes as its
# argument list: no warning, no crash, just garbage in the formatted output.
#
# Asserts the forwarded formatting produces the exact expected string and byte
# count, and that va_arg consumption still works in the same function shape. A
# looser "it ran" check would have passed while printing garbage.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] varargs_forward: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if ! "$AE" build "$SCRIPT_DIR/probe.ae" -o "$TMP/probe" >"$TMP/build.log" 2>&1; then
    echo "  [FAIL] varargs_forward: probe did not build"
    sed 's/^/        /' "$TMP/build.log" | head -15
    exit 1
fi

out=$("$TMP/probe" 2>&1)
if [ "$out" != "ok" ]; then
    echo "  [FAIL] varargs_forward: probe printed '$out', want 'ok'"
    exit 1
fi

# The va_list argument must be dereferenced at the boundary; without this the
# test above could only fail by luck of what the garbage bytes formatted to.
"$ROOT/build/aetherc" "$SCRIPT_DIR/probe.ae" "$TMP/probe.gen.c" >/dev/null 2>&1
if ! grep -q '\*(va_list\*)' "$TMP/probe.gen.c"; then
    echo "  [FAIL] varargs_forward: the forwarded va_list is not dereferenced at the call"
    grep -n 'vsnprintf' "$TMP/probe.gen.c" | head -5 | sed 's/^/        /'
    exit 1
fi

echo "  [PASS] varargs_forward: variadic tail forwards to a C v* function intact"
