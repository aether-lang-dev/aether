#!/bin/sh
# A `|` between match/switch selector values is the bitwise OR, not an
# alternative: `1 | 2 | 3 -> "small"` compiled and matched only 3, silently.
# Alternatives are a comma-list. The parser refuses the `|` form and names
# the spelling that was meant; the comma-list still works.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AETHERC" ] && [ ! -x "$AETHERC.exe" ]; then
    echo "  [SKIP] match_pipe_selector_reject: $AETHERC not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fail=0

cat > "$tmp/pipe.ae" <<'AE'
classify(x: int) -> string {
    return match x {
        1 | 2 | 3 -> "small"
        _ -> "other"
    }
}
main() { println(classify(2)) }
AE
out="$("$AETHERC" "$tmp/pipe.ae" "$tmp/pipe.c" 2>&1)"
if ! printf '%s' "$out" | grep -q "is the bitwise OR, not an alternative"; then
    echo "  [FAIL] match_pipe_selector_reject: the | form was not refused"
    printf '%s\n' "$out" | head -4 | sed 's/^/        /'
    fail=1
fi
if ! printf '%s' "$out" | grep -q "pipe.ae:3:"; then
    echo "  [FAIL] match_pipe_selector_reject: the diagnostic does not point at the arm"
    fail=1
fi
if ! printf '%s' "$out" | grep -q "1, 2, 3 ->"; then
    echo "  [FAIL] match_pipe_selector_reject: the diagnostic does not name the comma-list spelling"
    fail=1
fi

cat > "$tmp/comma.ae" <<'AE'
classify(x: int) -> string {
    return match x {
        1, 2, 3 -> "small"
        _ -> "other"
    }
}
main() { println("${classify(1)} ${classify(2)} ${classify(3)} ${classify(4)}") }
AE
got="$("$AE" run "$tmp/comma.ae" 2>&1 | tail -1)"
if [ "$got" != "small small small other" ]; then
    echo "  [FAIL] match_pipe_selector_reject: the comma-list no longer matches (got '$got')"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] match_pipe_selector_reject: | in a selector refused with the comma-list spelling; the comma-list matches"
fi
exit $fail
