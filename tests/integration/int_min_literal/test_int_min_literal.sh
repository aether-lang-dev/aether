#!/bin/bash
# INT64_MIN and INT32_MIN reach the generated C as constants it compiles
# without a warning.
#
# Aether has no negative literal; the minimums are written `(0 -
# 9223372036854775807) - 1` and `(0 - 2147483647) - 1`, and the constant
# folder turns each into the literal text `-9223372036854775808` /
# `-2147483648`, which codegen emitted as is. In C that is `-` applied to a
# positive literal: 9223372036854775808 fits no signed type, so clang warns
# it is taken as unsigned (-Wimplicitly-unsigned-literal) and gcc that it is
# "so large that it is unsigned"; 2147483648 does not fit an int, so the
# expression is a long and `printf("%d", ...)` of it is a -Wformat warning.
# CI builds with -Werror. Both are now spelled as <stdint.h> spells them:
# `(-9223372036854775807LL - 1)`, `(-2147483647 - 1)`.
#
# int_min.ae checks the values (it is in the .ae sweep too); this script
# also compiles its generated C with -Wall -Wextra -Werror.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AETHERC" ] || [ ! -x "$AE" ]; then
    echo "  [SKIP] int_min_literal: compiler not built"
    exit 0
fi
CC="${CC:-cc}"
command -v "$CC" >/dev/null 2>&1 || { echo "  [SKIP] int_min_literal: no C compiler"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run int_min.ae 2>"$TMP/run.err" )
got=$(printf '%s\n' "$out" | tr '\n' ' ' | sed 's/ *$//')
if [ "$got" != "-2147483648 -9223372036854775808" ]; then
    echo "  [FAIL] int_min_literal: the program printed:"
    { echo "$out"; cat "$TMP/run.err"; } | head -20 | sed 's/^/          /'
    fail=1
fi

if ! "$AETHERC" "$SCRIPT_DIR/int_min.ae" "$TMP/int_min.c" >"$TMP/aetherc.log" 2>&1; then
    echo "  [FAIL] int_min_literal: aetherc rejected int_min.ae"
    head -20 "$TMP/aetherc.log" | sed 's/^/          /'
    exit 1
fi

# No bare minimum integer literal anywhere in the C (the expected strings
# aside). A float literal with those digits, like the saturation bounds of
# the float to int helpers (#3524, `-2147483648.0`), is a double, not the
# unsigned or wider integer a bare minimum literal turns into.
bare=$(grep -nE -- '(^|[^"0-9])-(9223372036854775808|2147483648)([^"0-9.eE]|$)' "$TMP/int_min.c")
if [ -n "$bare" ]; then
    echo "  [FAIL] int_min_literal: the C has a bare minimum literal:"
    echo "$bare" | head -10 | sed 's/^/          /'
    fail=1
fi

# The compile flags `ae cflags` gives, without its link flags.
cflags=$("$AE" cflags | tr ' ' '\n' | grep -v '^-[lL]' | tr '\n' ' ')
# shellcheck disable=SC2086
if ! "$CC" -c -Wall -Wextra -Wno-unused -Werror $cflags "$TMP/int_min.c" -o "$TMP/int_min.o" >"$TMP/cc.log" 2>&1; then
    echo "  [FAIL] int_min_literal: $CC -Wall -Wextra -Werror on the generated C:"
    grep -E 'error|warning' "$TMP/cc.log" | head -10 | sed 's/^/          /'
    fail=1
fi

[ "$fail" -eq 0 ] || exit 1
echo "  [PASS] int_min_literal: INT64_MIN and INT32_MIN emit as warning-free C"
exit 0
