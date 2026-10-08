#!/bin/sh
# print's literal is a printf format, with `%%` for a percent sign. A
# conversion with no argument read whatever the C stack held:
# print("100% done\n") printed `100 1501462000one` and print("a %s\n")
# crashed. Such a conversion is now a compile error with a hint, and a
# print whose only argument is a literal is written as decoded text, with
# no printf at run time, so `%%` reads the same with or without a NUL in
# the literal.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] print_format_args: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

fail() {
    echo "  [FAIL] print_format_args: $1"
    [ -n "$2" ] && [ -f "$2" ] && sed 's/^/        /' "$2" | head -12
    exit 1
}

# expect_error <fixture> <spec>: compiling fails, naming the conversion
# and saying how to write a percent sign.
expect_error() {
    if "$AETHERC" "$SCRIPT_DIR/$1.ae" "$tmp/$1.c" >"$tmp/$1.log" 2>&1; then
        fail "$1.ae compiled, but '$2' has no argument" "$tmp/$1.log"
    fi
    grep -qF "print format '$2' has no argument" "$tmp/$1.log" ||
        fail "$1.ae: no diagnostic naming '$2'" "$tmp/$1.log"
    grep -qF "write '%%' for a literal '%'" "$tmp/$1.log" ||
        fail "$1.ae: the diagnostic has no hint" "$tmp/$1.log"
}

expect_error bad_no_argument '% d'
expect_error bad_second_argument_missing '%s'

"$AE" run "$SCRIPT_DIR/ok_literal_formats.ae" >"$tmp/ok.out" 2>"$tmp/ok.err" ||
    fail "ok_literal_formats.ae did not run" "$tmp/ok.err"
got="$(tr -d '\r' < "$tmp/ok.out" | od -An -c | tr -s ' \n' ' ')"
want="$(printf '100%% done\na%%\000b\n%%%%\n7 = 7%%\n' | od -An -c | tr -s ' \n' ' ')"
[ "$got" = "$want" ] || {
    echo "        want:$want"
    echo "        got: $got"
    fail "ok_literal_formats.ae printed other bytes"
}

echo "  [PASS] print_format_args: a conversion with no argument is refused; literal formats print their decoded text"
