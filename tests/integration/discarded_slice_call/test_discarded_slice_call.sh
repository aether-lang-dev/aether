#!/bin/sh
# Regression (#3821): a call whose slice result is discarded compiles clean
# under clang's -Werror=unused-value.
#
# The call's slice is held in a statement temporary, emitted as a comma
# expression that ends in the temporary; as a statement, clang reported the
# value unused ("expression result unused"), and projects that build with
# warnings as errors (ae3d on macOS) failed on 0.937. GCC is silent on it,
# so the C is compiled with clang, or zig cc (its clang), and the test skips
# without either.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
EXE=""
[ -x "$ROOT/build/aetherc" ] || EXE=".exe"
AETHERC="$ROOT/build/aetherc$EXE"
if [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] discarded_slice_call: compiler not built"
    exit 0
fi

CLANG=""
if command -v clang > /dev/null 2>&1; then
    CLANG="clang"
else
    . "$ROOT/tests/lib/find_zig.sh"
    z="$(find_zig "$ROOT")"
    [ -n "$z" ] && CLANG="$z cc"
fi
if [ -z "$CLANG" ]; then
    echo "  [SKIP] discarded_slice_call: no clang or zig here"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
if ! AETHER_HOME="" "$AETHERC" "$SCRIPT_DIR/discarded.ae" "$TMP/discarded.c" > "$TMP/gen.log" 2>&1; then
    echo "  [FAIL] discarded_slice_call: aetherc failed"
    sed 's/^/        /' "$TMP/gen.log" | head -10
    exit 1
fi
INC=""
for d in runtime runtime/actors runtime/scheduler runtime/memory runtime/config runtime/utils \
         include std std/string std/collections; do
    INC="$INC -I$ROOT/$d"
done
# Compiled to an object, not -fsyntax-only: zig cc on Windows answers a
# clean -fsyntax-only with FileNotFound (#3810).
# shellcheck disable=SC2086
if ! $CLANG -c -o "$TMP/discarded.o" -Werror=unused-value -Wno-unused-command-line-argument $INC \
        "$TMP/discarded.c" > "$TMP/cc.log" 2>&1; then
    echo "  [FAIL] discarded_slice_call: clang reports the generated C"
    grep -E "error|warning" "$TMP/cc.log" | head -5 | cut -c1-220 | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] discarded_slice_call: a discarded slice-returning call is clean under clang -Werror=unused-value"
exit 0
