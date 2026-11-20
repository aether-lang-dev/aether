#!/bin/sh
# `ae pad` (#3775): the layout report of structs, messages and actor state.
#
# pad_fixture.ae declares structs whose layout is known by hand (the issue's
# Rec, 40 bytes in its order and 24 by alignment; nested structs, a fixed
# array, optionals, a sum, an extern struct, a @packed one), a message and an
# actor. The text and JSON reports must match the goldens beside this
# script; --verify must compile the program's C with a _Static_assert of
# every number and pass; --min-waste must gate on the largest saving; --fix
# must rewrite the reorderable structs to pad_fixture.fixed exactly, the
# program must print what it printed before, and a second --fix must change
# nothing. pad_refuse.ae holds a struct for each reason --fix refuses, and
# none of them may move.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
EXE=""
[ -x "$ROOT/build/ae" ] || EXE=".exe"
AE="$ROOT/build/ae$EXE"
if [ ! -x "$AE" ] || [ ! -x "$ROOT/build/aetherc$EXE" ]; then
    echo "  [SKIP] ae_pad: compiler not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0
cd "$TMP" || exit 1
cp "$SCRIPT_DIR/pad_fixture.ae" "$SCRIPT_DIR/pad_refuse.ae" .

# same <what> <expected file> <actual file>: line endings aside.
same() {
    tr -d '\r' < "$2" > "$TMP/expected.txt"
    tr -d '\r' < "$3" > "$TMP/actual.txt"
    if [ "$(cat "$TMP/expected.txt")" != "$(cat "$TMP/actual.txt")" ]; then
        echo "  [FAIL] ae_pad: $1 differs from $(basename "$2")"
        if command -v diff > /dev/null 2>&1; then
            diff "$TMP/expected.txt" "$TMP/actual.txt" | head -20 | sed 's/^/        /'
        else
            head -20 "$TMP/actual.txt" | sed 's/^/        /'
        fi
        fail=1
    fi
}

# 1. The reports.
"$AE" pad pad_fixture.ae > text.out 2> text.err
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] ae_pad: the text report exited $rc"
    sed 's/^/        /' text.err | head -10
    fail=1
fi
same "the text report" "$SCRIPT_DIR/pad_fixture.txt" text.out
"$AE" pad --json pad_fixture.ae > json.out 2> json.err || {
    echo "  [FAIL] ae_pad: the JSON report failed"
    fail=1
}
same "the JSON report" "$SCRIPT_DIR/pad_fixture.json" json.out

# 2. --verify: the numbers hold for the C compiler.
if "$AE" pad --verify pad_fixture.ae > verify.out 2>&1; then
    if ! grep -q "^verified: pad_fixture.ae (64 asserts)" verify.out; then
        echo "  [FAIL] ae_pad: --verify did not check the 64 numbers"
        sed 's/^/        /' verify.out | head -10
        fail=1
    fi
else
    echo "  [FAIL] ae_pad: --verify failed: the C compiler disagrees with the table"
    sed 's/^/        /' verify.out | head -20
    fail=1
fi
# The 32-bit table, where the compiler can target 32-bit x86.
cat > probe32.c <<'C'
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
_Static_assert(sizeof(void*) == 4, "32-bit");
C
CC="${AE_CC:-${CC:-gcc}}"
if "$CC" -m32 -fsyntax-only probe32.c > /dev/null 2>&1; then
    if ! "$AE" pad --verify --ptr=4 pad_fixture.ae > verify32.out 2>&1; then
        echo "  [FAIL] ae_pad: --verify --ptr=4 failed: the C compiler disagrees with the 32-bit table"
        sed 's/^/        /' verify32.out | head -20
        fail=1
    fi
else
    echo "  [SKIP] ae_pad: --verify --ptr=4 ($CC does not take -m32 here)"
fi

# 3. --min-waste: Rec saves 16 bytes, the largest saving.
"$AE" pad --min-waste=16 pad_fixture.ae > /dev/null 2> gate.err
if [ $? -ne 1 ] || ! grep -q "struct Rec in pad_fixture.ae can save 16 bytes" gate.err; then
    echo "  [FAIL] ae_pad: --min-waste=16 did not fail on Rec"
    fail=1
fi
if ! "$AE" pad --min-waste=17 pad_fixture.ae > /dev/null 2>&1; then
    echo "  [FAIL] ae_pad: --min-waste=17 failed though no struct saves 17 bytes"
    fail=1
fi

# 4. --fix, then the same program.
AETHER_HOME= "$AE" run pad_fixture.ae > run_before.out 2>&1 || {
    echo "  [FAIL] ae_pad: the fixture does not run"
    sed 's/^/        /' run_before.out | head -10
    fail=1
}
"$AE" pad --fix pad_fixture.ae > fix.out 2>&1 || {
    echo "  [FAIL] ae_pad: --fix failed"
    sed 's/^/        /' fix.out | head -10
    fail=1
}
grep -q "^ae pad --fix: 4 structs rewritten" fix.out || {
    echo "  [FAIL] ae_pad: --fix did not rewrite Rec, Outer, Arr and Maybe"
    sed 's/^/        /' fix.out | head -10
    fail=1
}
same "the rewritten source" "$SCRIPT_DIR/pad_fixture.fixed" pad_fixture.ae
AETHER_HOME= "$AE" run pad_fixture.ae > run_after.out 2>&1 || {
    echo "  [FAIL] ae_pad: the rewritten fixture does not run"
    sed 's/^/        /' run_after.out | head -10
    fail=1
}
same "the rewritten program's output" run_before.out run_after.out
cp pad_fixture.ae fixed_once.ae
"$AE" pad --fix pad_fixture.ae > fix2.out 2>&1
grep -q "^ae pad --fix: 0 structs rewritten" fix2.out || {
    echo "  [FAIL] ae_pad: a second --fix rewrote something"
    fail=1
}
same "the source after a second --fix" fixed_once.ae pad_fixture.ae
"$AE" pad --min-waste=1 pad_fixture.ae > /dev/null 2>&1 || {
    echo "  [FAIL] ae_pad: after --fix a reorderable struct still saves bytes"
    fail=1
}

# 5. Refusals: each struct says why, and --fix moves none of them.
"$AE" pad pad_refuse.ae > refuse.out 2>&1
for why in \
    "struct Imported  pad_refuse.ae:6  layout unknown: @c_import: the C header lays it out" \
    "struct Opaque  pad_refuse.ae:12  layout unknown: extern type: opaque" \
    "reorderable: no (passed to extern fn takes_to_c (line 20), which C implements)" \
    "reorderable: no (in the signature of fn read_exported (line 28), which an --emit=lib build exports to C)" \
    "reorderable: no (carried by message Carry (line 38), whose struct --emit-header gives C hosts)" \
    "reorderable: no (conservative: the file reads raw offsets with std.mem" \
    "reorderable: no (@derive(schema): its field table follows declaration order)" \
    "reorderable: no (@observable: observers are told field indices in declaration order)" \
    "reorderable: no (held by extern struct Wrap (line 67), which C lays out)" \
    "reorderable: no (extern struct: the C side declares this order)"; do
    if ! grep -qF "$why" refuse.out; then
        echo "  [FAIL] ae_pad: pad_refuse.ae does not report: $why"
        fail=1
    fi
done
cp pad_refuse.ae refuse_before.ae
"$AE" pad --fix pad_refuse.ae > refuse_fix.out 2>&1
grep -q "^ae pad --fix: 0 structs rewritten" refuse_fix.out || {
    echo "  [FAIL] ae_pad: --fix rewrote a struct it must refuse"
    sed 's/^/        /' refuse_fix.out | head -10
    fail=1
}
same "pad_refuse.ae after --fix" refuse_before.ae pad_refuse.ae
"$AE" pad --min-waste=1 pad_refuse.ae > /dev/null 2>&1 || {
    echo "  [FAIL] ae_pad: --min-waste counted a struct --fix may not rewrite"
    fail=1
}

# 6. A layout --fix cannot follow is left alone, said, and fails the run.
cat > shared_line.ae <<'AE'
struct Mixed {
    a: byte  b: long
    c: byte
}

main() {
    m = Mixed { a: 1, b: 2, c: 3 }
    println("${m.b}")
}
AE
cp shared_line.ae shared_before.ae
"$AE" pad --fix shared_line.ae > shared.out 2>&1
rc=$?
if [ "$rc" -eq 0 ] || ! grep -q "struct Mixed left as it is: two fields share a line" shared.out; then
    echo "  [FAIL] ae_pad: --fix on two fields sharing a line was not refused (exit $rc)"
    sed 's/^/        /' shared.out | head -10
    fail=1
fi
same "shared_line.ae after a refused --fix" shared_before.ae shared_line.ae

if [ "$fail" -eq 0 ]; then
    echo "  [PASS] ae_pad: report, JSON, --verify, --min-waste, --fix round trip, refusals"
fi
exit "$fail"
