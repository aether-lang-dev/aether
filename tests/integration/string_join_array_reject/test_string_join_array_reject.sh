#!/bin/sh
# A string[] given to a *StringSeq parameter is a type error at the call, and
# a [...] literal there is built as a sequence.
#
# Reported while building testcontainers-ae: `string.join(xs, ",")` with
# `xs: string[]` type-checked against `join(s: *StringSeq, sep)`, because an
# array decayed to ANY typed pointer, and the callee walked the C array as
# cons cells: it printed only the last element, or crashed. The decay is now
# C's, to a pointer to the element only, and the call is E0200 naming
# string.seq_from_strings and string.join_strings. A `[...]` literal of
# strings given to the same parameter is a sequence literal, as it already
# was where a variable, a field or a message field of that type takes one,
# so `string.join(["a", "b", "c"], ",")` is "a,b,c".

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] string_join_array_reject: ae not built"; exit 0; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0

expect_error() {  # file, fixed string, label
    out="$(cd "$tmp" && AETHER_HOME="$ROOT" "$AE" check "$1" 2>&1)"
    if ! printf '%s' "$out" | grep -qF "$2"; then
        echo "  [FAIL] string_join_array_reject: $3"
        printf '%s\n' "$out" | head -8 | sed 's/^/        /'
        fail=1
    fi
}

# The report's shape: a string[] local.
cat > "$tmp/local.ae" <<'AE'
import std.string
main() {
    xs = ["a", "b", "c"]
    println(string.join(xs, ","))
}
AE
expect_error local.ae "expected *StringSeq, got array (a string[] is not a *StringSeq" \
    "a string[] local given to string.join was not refused"
expect_error local.ae "string.join_strings(xs, sep)" \
    "the refusal did not name string.join_strings"

# A string[] parameter (a slice).
cat > "$tmp/param.ae" <<'AE'
import std.string
show(xs: string[]) -> string { return string.join(xs, ",") }
main() { println(show(["a"])) }
AE
expect_error param.ae "expected *StringSeq, got" \
    "a string[] parameter given to string.join was not refused"

# Any typed pointer, not only *StringSeq: an int[] into a user *Box.
cat > "$tmp/typed.ae" <<'AE'
struct Box { n: int }
size_of(b: *Box) -> int { return b.n }
main() {
    xs = [1, 2, 3]
    println("${size_of(xs)}")
}
AE
expect_error typed.ae "Argument 1 'b' of 'size_of': expected *Box, got" \
    "an int[] given to a *Box parameter was not refused"

# What stays legal, built and run: a literal into join (a sequence), the
# string[] through join_strings and seq_from_strings, and an array into a
# bare `ptr` (the C decay #892 relies on).
cat > "$tmp/ok.ae" <<'AE'
import std.string
extern memchr(s: ptr, c: int, n: int) -> ptr
main() {
    xs = ["a", "b", "c"]
    byte[4] bs = [7, 8, 9, 10]
    hit = memchr(bs, 9, 4)
    found = 0
    if hit != null { found = 1 }
    println("${string.join(["a", "b", "c"], ",")} ${string.join_strings(xs, "-")} ${string.join(string.seq_from_strings(xs), "+")} ${found}")
}
AE
got="$(cd "$tmp" && AETHER_HOME="$ROOT" "$AE" run ok.ae 2>&1 | tail -1)"
if [ "$got" != "a,b,c a-b-c a+b+c 1" ]; then
    echo "  [FAIL] string_join_array_reject: a legal call was refused or misread (got '$got')"
    (cd "$tmp" && AETHER_HOME="$ROOT" "$AE" run ok.ae 2>&1 | head -8 | sed 's/^/        /')
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] string_join_array_reject: string[] to *StringSeq refused with the fix named; a literal joins every element"
fi
exit $fail
