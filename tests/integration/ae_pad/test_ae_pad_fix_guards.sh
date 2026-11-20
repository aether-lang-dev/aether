#!/bin/sh
# `ae pad --fix` (#3775) leaves alone what it cannot rewrite safely.
#
# - A module's struct that a program of the same run hands to C, by an
#   extern or in a message, is refused: the run decides over every file, not
#   one at a time. A module no program of the run imports is refused too,
#   since its users are not known; one only used locally is rewritten.
# - A *S that becomes an untyped ptr (`h as ptr`, a `ptr` parameter) refuses
#   S; one only freed does not.
# - A body whose last field shares the closing brace's line, or with a
#   block comment over several lines, is left as written and the run fails;
#   `/*/` opens a comment, as the lexer reads it.
# - The file keeps its mode, a file named like the old temporary is not
#   touched, a source holding a NUL byte is refused, --json does not mix
#   with --fix or --verify, and a symlinked directory is not walked.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
EXE=""
[ -x "$ROOT/build/ae" ] || EXE=".exe"
AE="$ROOT/build/ae$EXE"
if [ ! -x "$AE" ] || [ ! -x "$ROOT/build/aetherc$EXE" ]; then
    echo "  [SKIP] ae_pad_fix_guards: compiler not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0

# guard_same <what> <file a> <file b>: the same text, line endings aside.
guard_same() {
    if [ "$(tr -d '\r' < "$2")" != "$(tr -d '\r' < "$3")" ]; then
        echo "  [FAIL] ae_pad_fix_guards: $1"
        if command -v diff > /dev/null 2>&1; then
            diff "$2" "$3" | head -20 | sed 's/^/        /'
        fi
        fail=1
    fi
}

# guard_has <what> <file> <text>
guard_has() {
    if ! grep -qF "$3" "$2"; then
        echo "  [FAIL] ae_pad_fix_guards: $1: no \"$3\""
        sed 's/^/        /' "$2" | head -20
        fail=1
    fi
}

# guard_project <dir> <main.ae body>: src/geom/module.ae declares Vec,
# src/main.ae is the program given.
guard_project() {
    mkdir -p "$TMP/$1/src/geom"
    printf '[package]\nname = "%s"\nversion = "0.1.0"\n' "$1" > "$TMP/$1/aether.toml"
    cat > "$TMP/$1/src/geom/module.ae" <<'AE'
// A module whose struct would be smaller in another order.
exports(Vec)

struct Vec {
    a: byte
    b: long
    c: byte
}
AE
    cat > "$TMP/$1/src/main.ae"
    cp "$TMP/$1/src/geom/module.ae" "$TMP/$1/module_before.ae"
}

# 1. Across files.
guard_project extern <<'AE'
import geom

extern draw(v: *Vec) -> int

main() {
    v = Vec { a: 1, b: 2, c: 3 }
    println("${v.b}")
}
AE
(cd "$TMP/extern" && "$AE" pad --fix src > fix.out 2>&1)
guard_has "a module's struct a program passes to an extern" "$TMP/extern/fix.out" \
    "src/geom/module.ae:4: struct Vec not rewritten: passed to extern fn draw (line 3), which C implements (seen from src/main.ae)"
guard_same "Vec was rewritten though src/main.ae passes it to C" \
    "$TMP/extern/module_before.ae" "$TMP/extern/src/geom/module.ae"

guard_project message <<'AE'
import geom

message Carry {
    item: Vec
}

main() {
    v = Vec { a: 1, b: 2, c: 3 }
    println("${v.b}")
}
AE
(cd "$TMP/message" && "$AE" pad --fix src > fix.out 2>&1)
guard_has "a module's struct a program's message carries" "$TMP/message/fix.out" \
    "struct Vec not rewritten: carried by message Carry (line 3), whose struct --emit-header gives C hosts (seen from src/main.ae)"
guard_same "Vec was rewritten though a message of src/main.ae carries it" \
    "$TMP/message/module_before.ae" "$TMP/message/src/geom/module.ae"

(cd "$TMP/extern" && "$AE" pad --fix src/geom > alone.out 2>&1)
guard_has "a module no program of the run imports" "$TMP/extern/alone.out" \
    "struct Vec not rewritten: a module's users outside this run are not known"
guard_same "Vec was rewritten with no program of the run to see it" \
    "$TMP/extern/module_before.ae" "$TMP/extern/src/geom/module.ae"

guard_project local <<'AE'
import geom

main() {
    v = Vec { a: 1, b: 2, c: 3 }
    println("${v.b}")
}
AE
(cd "$TMP/local" && "$AE" pad --fix src > fix.out 2>&1) || {
    echo "  [FAIL] ae_pad_fix_guards: --fix of a module a program uses locally failed"
    sed 's/^/        /' "$TMP/local/fix.out" | head -10
    fail=1
}
guard_has "a module's struct used only in Aether" "$TMP/local/fix.out" \
    "src/geom/module.ae:4: struct Vec reordered to b, a, c (24 -> 16 bytes)"

# 2. A *S that becomes an untyped ptr.
mkdir -p "$TMP/untyped" && cd "$TMP/untyped" || exit 1
cat > untyped.ae <<'AE'
struct Hdr {
    a: byte
    b: long
    c: byte
}

struct Passed {
    a: byte
    b: long
    c: byte
}

struct Freed {
    a: byte
    b: long
    c: byte
}

extern fwrite(p: ptr, size: int, n: int, f: ptr) -> int

keep(p: ptr) -> int {
    return 1
}

main() {
    h = heap.new(Hdr)
    if h.b > 1 {
        fwrite(h as ptr, 24, 1, null)
    }
    q = heap.new(Passed)
    println("${keep(q)}")
    f = heap.new(Freed)
    f.b = 3
    println("${f.b}")
    heap.free(f)
}
AE
cp untyped.ae untyped_before.ae
"$AE" pad untyped.ae > report.out 2>&1
guard_has "a *Hdr cast to ptr" report.out \
    "reorderable: no (conservative: a *Hdr becomes an untyped ptr (line 28), which C may read at offsets of its own)"
guard_has "a *Passed passed as a ptr" report.out \
    "reorderable: no (conservative: a *Passed is passed to keep (line 31), which takes it as something other than *Passed)"
"$AE" pad --fix untyped.ae > fix.out 2>&1
guard_has "a *Freed that is only freed" fix.out "untyped.ae:13: struct Freed reordered to b, a, c (24 -> 16 bytes)"
guard_has "only Freed is rewritten" fix.out "ae pad --fix: 1 struct rewritten"

# 3, 4, 5. Bodies --fix cannot follow, and `/*/`.
mkdir -p "$TMP/bodies" && cd "$TMP/bodies" || exit 1
# guard_refused <name> <reason>: <name>.ae, already written, is refused and unchanged.
guard_refused() {
    cp "$1.ae" "$1_before.ae"
    if "$AE" pad --fix "$1.ae" > "$1.out" 2>&1; then
        echo "  [FAIL] ae_pad_fix_guards: --fix of $1.ae succeeded (should refuse)"
        fail=1
    fi
    guard_has "$1.ae refused" "$1.out" "left as it is: $2"
    guard_same "$1.ae changed though refused" "$1_before.ae" "$1.ae"
}
printf 'struct B {\n    a: byte\n    b: long\n    c: byte }\n\nmain() {\n    x = B { a: 1, b: 2, c: 3 }\n    println("${x.b}")\n}\n' > brace.ae
guard_refused brace "a field shares its line with the closing brace"
printf 'struct B {\n    a: byte,\n    b: long,\n    c: byte, }\n\nmain() {\n    x = B { a: 1, b: 2, c: 3 }\n    println("${x.b}")\n}\n' > brace_comma.ae
guard_refused brace_comma "a field shares its line with the closing brace"
printf 'struct C {\n    a: byte /* a comment that\n    goes on */\n    b: long\n    c: byte\n}\n\nmain() {\n    x = C { a: 1, b: 2, c: 3 }\n    println("${x.b}")\n}\n' > block.ae
guard_refused block "a block comment spans lines"
printf 'struct D {\n    a: byte /*/ b: long, still the comment */\n    b: long\n    c: byte\n}\n\nmain() {\n    x = D { a: 1, b: 2, c: 3 }\n    println("${x.b}")\n}\n' > slash.ae
printf 'struct D {\n    b: long\n    a: byte /*/ b: long, still the comment */\n    c: byte\n}\n\nmain() {\n    x = D { a: 1, b: 2, c: 3 }\n    println("${x.b}")\n}\n' > slash_expected.ae
"$AE" pad --fix slash.ae > slash.out 2>&1 || {
    echo "  [FAIL] ae_pad_fix_guards: --fix of a body with /*/ failed"
    sed 's/^/        /' slash.out | head -10
    fail=1
}
guard_same "/*/ read as a whole comment" slash_expected.ae slash.ae

# 7, 8, 9. The file, its mode, the temporary, a NUL byte, --json.
mkdir -p "$TMP/files" && cd "$TMP/files" || exit 1
cp "$SCRIPT_DIR/pad_fixture.ae" fx.ae
chmod 600 fx.ae
mode_before="$(ls -l fx.ae | cut -c1-10)"
printf 'keep\n' > fx.ae.pad-tmp
"$AE" pad --fix fx.ae > fix.out 2>&1 || { echo "  [FAIL] ae_pad_fix_guards: --fix of fx.ae failed"; fail=1; }
guard_has "the fixture is rewritten" fix.out "ae pad --fix: 4 structs rewritten"
if [ "$(ls -l fx.ae | cut -c1-10)" != "$mode_before" ]; then
    echo "  [FAIL] ae_pad_fix_guards: --fix changed the file's mode ($mode_before to $(ls -l fx.ae | cut -c1-10))"
    fail=1
fi
if [ "$(cat fx.ae.pad-tmp 2>/dev/null)" != "keep" ]; then
    echo "  [FAIL] ae_pad_fix_guards: --fix clobbered fx.ae.pad-tmp"
    fail=1
fi
ls | grep -v -e '^fx\.ae$' -e '^fx\.ae\.pad-tmp$' -e '^fix\.out$' > ../stray.out
if [ -s ../stray.out ]; then
    echo "  [FAIL] ae_pad_fix_guards: --fix left files behind: $(tr '\n' ' ' < ../stray.out)"
    fail=1
fi
{ cat "$SCRIPT_DIR/pad_fixture.ae"; printf '// a NUL \000 here\n'; } > nul.ae
cp nul.ae nul_before.ae
if "$AE" pad --fix nul.ae > nul.out 2>&1; then
    echo "  [FAIL] ae_pad_fix_guards: --fix of a file holding a NUL succeeded"
    fail=1
fi
if ! cmp_ok=$(cksum < nul.ae) || [ "$cmp_ok" != "$(cksum < nul_before.ae)" ]; then
    echo "  [FAIL] ae_pad_fix_guards: a file holding a NUL was rewritten"
    fail=1
fi
for combo in "--json --fix" "--json --verify"; do
    # shellcheck disable=SC2086
    "$AE" pad $combo fx.ae > json.out 2> json.err
    rc=$?
    if [ "$rc" -ne 2 ] || ! grep -q "^ae pad: --json is the report alone" json.err; then
        echo "  [FAIL] ae_pad_fix_guards: ae pad $combo was not refused (exit $rc)"
        fail=1
    fi
done

# 10. A symlinked directory is not walked (where the shell makes real links).
mkdir -p "$TMP/walk/tree/sub" && cd "$TMP/walk" || exit 1
printf 'main() {\n    println("x")\n}\n' > tree/sub/one.ae
# MSYS makes a copy for `ln -s` unless asked for a real link; a copy of the
# parent inside itself is not what is meant.
if MSYS=winsymlinks:nativestrict ln -s .. tree/sub/up 2> /dev/null && [ -L tree/sub/up ]; then
    "$AE" pad tree > walk.out 2>&1
    n="$(grep -c '^layout: ' walk.out)"
    if [ "$n" != "1" ]; then
        echo "  [FAIL] ae_pad_fix_guards: a symlinked directory was walked ($n reports for one file)"
        fail=1
    fi
    # A symlinked source is not replaced by a file.
    cp "$SCRIPT_DIR/pad_fixture.ae" real.ae
    MSYS=winsymlinks:nativestrict ln -s real.ae link.ae
    if "$AE" pad --fix link.ae > link.out 2>&1 || [ ! -L link.ae ]; then
        echo "  [FAIL] ae_pad_fix_guards: --fix rewrote through a symbolic link"
        fail=1
    fi
    guard_has "a symlinked source" link.out "is a symbolic link; --fix rewrites the file it names"
else
    echo "  [SKIP] ae_pad_fix_guards: no symbolic links here; the walk is not checked"
fi

if [ "$fail" -eq 0 ]; then
    echo "  [PASS] ae_pad_fix_guards: cross-file refusals, untyped ptr, bodies, file handling"
fi
exit "$fail"
