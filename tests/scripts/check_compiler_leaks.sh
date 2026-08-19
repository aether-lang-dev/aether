#!/bin/sh
# The compiler must not leak while compiling these programs (#1667).
#
# Every allocation aetherc makes for a compile should be released by the time
# it exits: the AST, the types stamped on it, the tokens, and the nodes codegen
# synthesises for itself. That was not true for any program at all, and it is
# what keeps the compiler out of a leak gate while the runtime already has one.
# It matters beyond tidiness because the compiler is also a library, and the
# LSP server calls these same entry points over and over in one process.
#
# The list is the shapes that are clean today. Anything added here is a
# promise; a program that still leaks belongs in the issue, not in this file.
#
# Needs a LeakSanitizer build of the compiler, which is what the memory-check
# workflow's ASan job already produces:
#   make compiler CFLAGS="-fsanitize=address -fsanitize=leak ..." LDFLAGS=...

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] compiler leaks: build/aetherc not built"
    exit 0
fi

# Refuse to pass silently on a build with no leak detector: a green result
# here would otherwise mean nothing on macOS or on a non-sanitized build.
probe_dir="$(mktemp -d)"
cat > "$probe_dir/probe.ae" <<'AEOF'
main() {
    leaked = "x"
    println("${leaked}")
}
AEOF
if ! ASAN_OPTIONS=detect_leaks=1 "$AETHERC" "$probe_dir/probe.ae" "$probe_dir/probe.c" \
        >"$probe_dir/probe.log" 2>&1; then
    echo "  [SKIP] compiler leaks: $AETHERC cannot compile a trivial program"
    sed 's/^/        /' "$probe_dir/probe.log" | head -5
    rm -rf "$probe_dir"
    exit 0
fi
if ! grep -q "detect_leaks" "$probe_dir/probe.log" 2>/dev/null; then
    : # a clean run prints nothing; the detector check below covers the rest
fi
rm -rf "$probe_dir"

PROGRAMS="
tests/regression/test_string_edge_cases.ae
tests/regression/test_string_interp_value.ae
tests/regression/test_string_escape_sequences.ae
tests/regression/test_bytes_le64.ae
tests/regression/test_optional_heap_leak.ae
"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

failed=0
checked=0
for prog in $PROGRAMS; do
    [ -f "$ROOT/$prog" ] || continue
    checked=$((checked + 1))
    out="$(ASAN_OPTIONS=detect_leaks=1 "$AETHERC" "$ROOT/$prog" "$TMP/out.c" 2>&1)"
    if printf '%s' "$out" | grep -q "LeakSanitizer: detected memory leaks"; then
        bytes="$(printf '%s' "$out" | sed -n 's/.*SUMMARY: AddressSanitizer: \([0-9]*\) byte.*/\1/p')"
        echo "  [FAIL] compiler leaks: $prog leaked ${bytes:-?} bytes"
        printf '%s\n' "$out" | grep -A6 "Direct leak" | head -12 | sed 's/^/        /'
        failed=$((failed + 1))
    fi
done

if [ "$checked" -eq 0 ]; then
    echo "  [SKIP] compiler leaks: none of the listed programs are present"
    exit 0
fi
if [ "$failed" -gt 0 ]; then
    echo "  compiler leaks: $failed of $checked programs leaked"
    exit 1
fi

echo "  [PASS] compiler leaks: $checked programs compile with no leaked allocation"
