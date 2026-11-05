#!/bin/sh
# Issue #1681: an output name that matches an input destroyed the input.
#
# `ae build` writes the generated C to `<output>.c` and the program to
# `<output>`, so `ae build p.ae --extra p.c -o p` wrote generated code over
# p.c and then failed to link against the source it had just overwritten.
#
# What this pins is not the wording of the refusal but the file: the inputs
# must be byte-for-byte what they were, and a build whose names do not collide
# must still work. The generated C now goes to the temp directory, so it can no
# longer land on an input at all: `-o p` beside `--extra p.c` builds, and p.c
# is untouched. The program is still written to `<output>`, so `-o p.ae` is
# still refused.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] build_output_clobbers_input: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

printf '#include <string.h>\nint probe_copy(int n) { return n; }\n' > "$TMP/p.c"
printf 'extern probe_copy(n: int) -> int\n\nmain() {\n    probe_copy(8)\n    println("ok")\n}\n' > "$TMP/p.ae"
c_before="$(cksum < "$TMP/p.c")"
ae_before="$(cksum < "$TMP/p.ae")"

# -o p once put the generated C at p.c, the --extra input. It builds now,
# with the generated C elsewhere, and p.c is what it was.
"$AE" build "$TMP/p.ae" --extra "$TMP/p.c" -o "$TMP/p" >"$TMP/out.log" 2>&1 \
    || { sed 's/^/        /' "$TMP/out.log" | head -10; fail "-o p beside --extra p.c did not build"; }
[ "$(cksum < "$TMP/p.c")" = "$c_before" ] \
    || fail "the --extra input was overwritten by the generated C"
[ "$("$TMP/p")" = "ok" ] || fail "the -o p program ran wrong"

# -o p.ae puts the program where the source is.
if "$AE" build "$TMP/p.ae" -o "$TMP/p.ae" >"$TMP/out2.log" 2>&1; then
    fail "the build succeeded while writing its program over the source"
fi
[ "$(cksum < "$TMP/p.ae")" = "$ae_before" ] \
    || fail "the Aether source was overwritten by the program"

# The same, with the input spelled relatively.
(cd "$TMP" && "$AE" build p.ae --extra ./p.c -o p >out3.log 2>&1) \
    || fail "-o p beside --extra ./p.c did not build"
[ "$(cksum < "$TMP/p.c")" = "$c_before" ] \
    || fail "the --extra input was overwritten when named relatively"

# And names that do not collide still build and run.
"$AE" build "$TMP/p.ae" --extra "$TMP/p.c" -o "$TMP/prog" >"$TMP/ok.log" 2>&1 \
    || { sed 's/^/        /' "$TMP/ok.log" | head -10; fail "a non-colliding build was rejected"; }
PROG="$(sed -n 's/^Built[^:]*: //p' "$TMP/ok.log" | tail -1)"
[ "$("$PROG")" = "ok" ] || fail "the non-colliding build ran wrong: $("$PROG")"

echo "  [PASS] build_output_clobbers_input: no output overwrites an input"
