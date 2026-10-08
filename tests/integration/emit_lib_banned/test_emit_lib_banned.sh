#!/bin/sh
# Verifies --emit=lib rejects every capability-heavy stdlib import.
# All five should produce the capability-empty error and a non-zero
# exit status.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT
pass=0; fail=0

check_banned() {
    mod="$1"
    printf 'import %s\nfoo(x: int) { return x }\n' "$mod" > "$TMPDIR/probe.ae"
    if AETHER_HOME="" "$ROOT/build/aetherc" --emit=lib "$TMPDIR/probe.ae" "$TMPDIR/probe.c" >"$TMPDIR/stdout" 2>"$TMPDIR/stderr"; then
        echo "  [FAIL] import $mod was accepted by --emit=lib"
        fail=$((fail + 1))
    elif grep -q "capability-empty" "$TMPDIR/stderr"; then
        echo "  [PASS] import $mod rejected"
        pass=$((pass + 1))
    else
        echo "  [FAIL] import $mod rejected, but without the expected 'capability-empty' message"
        cat "$TMPDIR/stderr"
        fail=$((fail + 1))
    fi
}

for mod in std.net std.http std.tcp std.fs std.os; do
    check_banned "$mod"
done

# Control: std.map must still be accepted (it's pure data).
printf 'import std.map\nfoo(x: int) { m = map_new(); return x }\n' > "$TMPDIR/ok.ae"
if AETHER_HOME="" "$ROOT/build/aetherc" --emit=lib "$TMPDIR/ok.ae" "$TMPDIR/ok.c" >"$TMPDIR/stdout" 2>"$TMPDIR/stderr"; then
    echo "  [PASS] import std.map accepted (capability-free)"
    pass=$((pass + 1))
else
    echo "  [FAIL] import std.map was rejected — expected success"
    cat "$TMPDIR/stderr"
    fail=$((fail + 1))
fi

echo ""
echo "emit_lib_banned: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
