#!/bin/sh
# Integration: `aetherc --diagnose=ownership` prints the same heap/non-heap
# verdicts the codegen wrapper terminator at codegen_stmt.c:1611-1631 will
# emit, without running codegen.
#
# Compiles a small fixture with both a heap-returning user fn (`my_concat`
# wrapping `string.concat`) and a non-heap one (`first_or_default` returning
# either a parameter borrow or a literal), plus assignments that exercise
# every RHS shape the diagnostic classifies.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] diagnose_ownership: aetherc not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cat > "$tmpdir/fixture.ae" <<'AE'
import std.string

my_concat(a: string, b: string) -> string {
    return string.concat(a, b)
}

first_or_default(a: string) -> string {
    if string.length(a) > 0 {
        return a
    }
    return "default"
}

main() {
    s = ""
    s = my_concat("hello ", "world")
    s = "literal"
    s = string.concat(s, "!")
}
AE

out="$tmpdir/diagnose.txt"
if ! "$AETHERC" --diagnose=ownership "$tmpdir/fixture.ae" >"$out" 2>&1; then
    echo "  [FAIL] diagnose_ownership: aetherc --diagnose=ownership exited non-zero"
    cat "$out"
    exit 1
fi

# Pass 1 — function verdicts.
if ! grep -qE "^  my_concat .* HEAP, every return path heap-classified" "$out"; then
    echo "  [FAIL] diagnose_ownership: my_concat should be classified HEAP"
    cat "$out"
    exit 1
fi
if ! grep -qE "^  first_or_default .* NOT HEAP" "$out"; then
    echo "  [FAIL] diagnose_ownership: first_or_default should be classified NOT HEAP"
    cat "$out"
    exit 1
fi

# Pass 2 — assignment verdicts. Each line ends with `_heap_<lhs> = N [shape]`.
# `s = ""`                      → 0 [literal]
# `s = my_concat(...)`           → 1 [heap-returning fn → HEAP]
# `s = "literal"`                → 0 [literal]
# `s = string.concat(s, "!")`    → 1 [heap-returning fn → HEAP]
expected_ones=$(grep -c "_heap_s = 1" "$out")
expected_zeros=$(grep -c "_heap_s = 0" "$out")
if [ "$expected_ones" != "2" ]; then
    echo "  [FAIL] diagnose_ownership: expected 2 '_heap_s = 1' lines, got $expected_ones"
    cat "$out"
    exit 1
fi
if [ "$expected_zeros" != "2" ]; then
    echo "  [FAIL] diagnose_ownership: expected 2 '_heap_s = 0' lines, got $expected_zeros"
    cat "$out"
    exit 1
fi

# UAF-triage stanza must be present so the porter knows what to do with the
# verdicts.
if ! grep -q "UAF triage" "$out"; then
    echo "  [FAIL] diagnose_ownership: missing UAF triage guidance stanza"
    cat "$out"
    exit 1
fi

# Dotted source-form `string.concat` must be recognised (the bug
# this whole change request closed). Without the dot-normalisation in
# is_heap_string_expr, the my_concat verdict above would be NOT HEAP.
echo "  [PASS] diagnose_ownership: heap/non-heap verdicts + dotted-call recognition + UAF stanza"
exit 0
