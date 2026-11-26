#!/bin/sh
# A multi-value return bound to ONE name is a tuple; passing that name where
# a parameter takes a scalar used to reach the C compiler ("incompatible type
# for argument 1 … argument is of type '_tuple_ptr_string'") instead of an
# Aether diagnostic. The front end now refuses it at the call, for user
# functions and externs alike, and names the fix (destructure). The
# destructured form still compiles and runs. A tuple-returning call written
# straight into the argument (or as a UFCS receiver) is refused the same way.

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

# The same mistake with the call written straight into the argument, no
# name in between: a user function, a module function reached through its
# qualified name, and a method-call (UFCS) receiver. Each used to reach the C
# compiler as "passing '_tuple_ptr_string' to parameter of incompatible type".
cat > "$tmp/call.ae" <<'AE'
import std.list
import std.spec

pair() -> {
    return "a", ""
}
takes_str(s: string) -> int { return 1 }

main() {
    xs = list.new()
    list.add(xs, "x")
    println("${takes_str(pair())}")
    spec.expect_str(list.get(xs, 0))
    pair().takes_str()
}
AE
out="$(AETHER_HOME="$ROOT" "$AETHERC" "$tmp/call.ae" "$tmp/call.c" 2>&1)"
if ! printf '%s' "$out" | grep -q "Argument 1 's' of 'takes_str': expected string, got the (string, string) returned by 'pair'"; then
    echo "  [FAIL] tuple_argument_reject: a tuple-returning call passed to a user function was not refused"
    printf '%s\n' "$out" | head -4 | sed 's/^/        /'
    fail=1
fi
if ! printf '%s' "$out" | grep -q "Argument 1 'v' of 'spec.expect_str': expected string, got the (ptr, string) returned by 'list.get'"; then
    echo "  [FAIL] tuple_argument_reject: a tuple-returning call passed to a module function was not refused"
    printf '%s\n' "$out" | head -4 | sed 's/^/        /'
    fail=1
fi
n="$(printf '%s' "$out" | grep -c "returned by 'pair'")"
if [ "$n" != 2 ]; then
    echo "  [FAIL] tuple_argument_reject: a tuple-returning UFCS receiver was not refused ($n of 2 reports)"
    fail=1
fi
if printf '%s' "$out" | grep -q "_tuple_"; then
    echo "  [FAIL] tuple_argument_reject: a generated-C type name leaked into the call report"
    fail=1
fi

cat > "$tmp/good.ae" <<'AE'
import std.list

pair() -> {
    return 41, "x"
}
takes_int(n: int) -> int { return n + 1 }
takes_str(s: string) -> int { return 0 }

main() {
    v, _ = pair()
    xs = list.new()
    list.add(xs, "x")
    s, _ = list.get_string(xs, 0)
    println("${takes_int(v) + takes_str(s)}")
}
AE
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/good.ae" 2>&1 | tail -1)"
if [ "$got" != "42" ]; then
    echo "  [FAIL] tuple_argument_reject: the destructured form no longer runs (got '$got')"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] tuple_argument_reject: a tuple, bound to a name or returned by a call, passed as a scalar is refused with the destructuring named"
fi
exit $fail
