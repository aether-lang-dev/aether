#!/bin/sh
# #2316: `ae fmt` writes the type prefixes tight.
#
# `[]T` (make's element type) and `[E]T` (an enum-indexed array) read as one
# type and the language reference spells them that way, but the formatter
# put a space after every `]` followed by a word: `make([] int, n)`,
# `[Dir] string`. A `]` that closes a prefix group (a `[` not following a
# value, holding nothing or one name) followed by a type name or `*` is now
# written tight. An index before a word (`a[0] as long`) and an array literal
# before a keyword (`[y] as ptr`) keep their space.
#
# The input and the expected output are here rather than as .ae files, so
# the fmt gate and the .ae sweep do not pick them up.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] fmt_type_prefix: ae not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

cat >"$TMPDIR/in.ae" <<'EOF'
enum Dir { N, S }
struct Node { v: int }
labels_of(n: int) -> [Dir] string {
    labels: [Dir] string = ["n", "s"]
    return labels
}
main() {
    m = make([] int, 2)
    p = make([] *Node, 2)
    q = make([]*Node, 1)
    x = [1, 2]
    y = m[0] as long
    z = [y] as ptr
    w: int[] = m
    idx = x[1] + m[0]
    e = [] as ptr
}
EOF

cat >"$TMPDIR/want.ae" <<'EOF'
enum Dir { N, S }
struct Node { v: int }
labels_of(n: int) -> [Dir]string {
    labels: [Dir]string = ["n", "s"]
    return labels
}
main() {
    m = make([]int, 2)
    p = make([]*Node, 2)
    q = make([]*Node, 1)
    x = [1, 2]
    y = m[0] as long
    z = [y] as ptr
    w: int[] = m
    idx = x[1] + m[0]
    e = [] as ptr
}
EOF

cp "$TMPDIR/in.ae" "$TMPDIR/got.ae"
if ! "$AE" fmt "$TMPDIR/got.ae" >"$TMPDIR/fmt.log" 2>&1; then
    echo "  [FAIL] fmt_type_prefix: ae fmt failed"
    sed 's/^/          /' "$TMPDIR/fmt.log" | head -10
    exit 1
fi
if [ "$(cat "$TMPDIR/want.ae")" != "$(cat "$TMPDIR/got.ae")" ]; then
    echo "  [FAIL] fmt_type_prefix: output differs from the expected layout; got:"
    sed 's/^/          /' "$TMPDIR/got.ae" | head -20
    exit 1
fi

# Formatting the formatted output changes nothing.
cp "$TMPDIR/got.ae" "$TMPDIR/again.ae"
"$AE" fmt "$TMPDIR/again.ae" >/dev/null 2>&1
if [ "$(cat "$TMPDIR/got.ae")" != "$(cat "$TMPDIR/again.ae")" ]; then
    echo "  [FAIL] fmt_type_prefix: formatting is not idempotent"
    exit 1
fi

echo "  [PASS] fmt_type_prefix: []T, []*T and [E]T tight; index and literal spacing kept"
exit 0
