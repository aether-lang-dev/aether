#!/bin/sh
# A multi-value return bound to ONE name is a tuple; passing that name where
# a parameter takes a scalar used to reach the C compiler ("incompatible type
# for argument 1 … argument is of type '_tuple_ptr_string'") instead of an
# Aether diagnostic. The front end now refuses it at the call, for user
# functions and externs alike, and names the fix (destructure). The
# destructured form still compiles and runs.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AETHERC" ] && [ ! -x "$AETHERC.exe" ]; then
    echo "  [SKIP] tuple_argument_reject: $AETHERC not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fail=0

cat > "$tmp/bad.ae" <<'AE'
import std.dir

pair() -> {
    return 1, "x"
}
takes_int(n: int) -> int { return n }

main() {
    l = dir.list(".")
    println("${dir.list_count(l)}")
    t = pair()
    println("${takes_int(t)}")
}
AE
out="$(AETHER_HOME="$ROOT" "$AETHERC" "$tmp/bad.ae" "$tmp/bad.c" 2>&1)"
if ! printf '%s' "$out" | grep -q "got the (ptr, string) tuple bound to 'l'"; then
    echo "  [FAIL] tuple_argument_reject: the tuple passed to an extern-backed call was not refused"
    printf '%s\n' "$out" | head -4 | sed 's/^/        /'
    fail=1
fi
if ! printf '%s' "$out" | grep -q "Argument 1 'n' of 'takes_int': expected int, got the (int, string) tuple bound to 't'"; then
    echo "  [FAIL] tuple_argument_reject: the tuple passed to a user function was not refused"
    printf '%s\n' "$out" | head -4 | sed 's/^/        /'
    fail=1
fi
if ! printf '%s' "$out" | grep -q "destructure it"; then
    echo "  [FAIL] tuple_argument_reject: the diagnostic does not name the fix"
    fail=1
fi
if printf '%s' "$out" | grep -q "_tuple_"; then
    echo "  [FAIL] tuple_argument_reject: a generated-C type name leaked into the report"
    fail=1
fi

cat > "$tmp/good.ae" <<'AE'
pair() -> {
    return 41, "x"
}
takes_int(n: int) -> int { return n + 1 }

main() {
    v, _ = pair()
    println("${takes_int(v)}")
}
AE
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/good.ae" 2>&1 | tail -1)"
if [ "$got" != "42" ]; then
    echo "  [FAIL] tuple_argument_reject: the destructured form no longer runs (got '$got')"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] tuple_argument_reject: a tuple-bound name passed as a scalar is refused with the destructuring named"
fi
exit $fail
