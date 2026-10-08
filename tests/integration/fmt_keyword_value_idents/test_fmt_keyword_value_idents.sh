#!/bin/sh
# #2542: `ae fmt` keeps binary arithmetic on `state`, `after` and `func`.
#
# The parser takes those three keywords as ordinary names (#880), but the
# formatter took a `-` or `+` after a keyword for a prefix operator, so
# `after - mid` came out as `after -mid` and `return func + 1` as
# `return func +1`. Their prefix uses keep their own spacing: `x = -after`.
#
# The input and the expected output are here rather than as .ae files, so
# the fmt gate and the .ae sweep do not pick them up.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] fmt_keyword_value_idents: ae not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

cat >"$TMPDIR/in.ae" <<'EOF'
struct N {
    func: int
    after: int
}
add(func: int, state: int, after: int) -> int {
    return func +1 + state -2 + after*3
}
main() {
    before = 1
    after = 4
    mid = 2
    n = N { func: 1, after: 2 }
    d = after -mid
    e = n.func +n.after
    f = -after
    g = after++
    println("${add(1, 2, 3)} ${d} ${e} ${f} ${g} ${before}")
}
EOF

cat >"$TMPDIR/want.ae" <<'EOF'
struct N {
    func: int
    after: int
}
add(func: int, state: int, after: int) -> int {
    return func + 1 + state - 2 + after * 3
}
main() {
    before = 1
    after = 4
    mid = 2
    n = N { func: 1, after: 2 }
    d = after - mid
    e = n.func + n.after
    f = -after
    g = after++
    println("${add(1, 2, 3)} ${d} ${e} ${f} ${g} ${before}")
}
EOF

cp "$TMPDIR/in.ae" "$TMPDIR/fmt.ae"
if ! "$AE" fmt "$TMPDIR/fmt.ae" >"$TMPDIR/fmt.log" 2>&1; then
    echo "  [FAIL] fmt_keyword_value_idents: ae fmt failed"
    sed 's/^/    /' "$TMPDIR/fmt.log"
    exit 1
fi
got="$(tr -d '\r' <"$TMPDIR/fmt.ae")"
want="$(cat "$TMPDIR/want.ae")"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] fmt_keyword_value_idents: formatted output differs; got:"
    printf '%s\n' "$got" | sed 's/^/    /'
    exit 1
fi
echo "  [PASS] fmt_keyword_value_idents: arithmetic on state, after and func stays binary"
exit 0
