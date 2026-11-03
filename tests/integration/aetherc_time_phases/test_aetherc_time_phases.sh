#!/bin/sh
# #3518: `aetherc --time-phases` writes one `phase <name> <ms>` line to stderr
# for each phase of the compile, in order, and the compile itself is the same
# as without the flag. benchmarks/core/run.ae reads these lines.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
. "$ROOT/tests/lib/same_bytes.sh"

[ -x "$AETHERC" ] || [ -x "$AETHERC.exe" ] || { echo "  [SKIP] aetherc_time_phases: $AETHERC not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT

cat > "$TMP/hello.ae" <<'AE'
main() {
    println("hello")
}
AE

if ! "$AETHERC" --time-phases "$TMP/hello.ae" "$TMP/timed.c" 2> "$TMP/err.txt"; then
    echo "  [FAIL] aetherc_time_phases: the compile failed"
    cat "$TMP/err.txt"
    exit 1
fi
"$AETHERC" "$TMP/hello.ae" "$TMP/plain.c" 2> /dev/null

names=$(sed -n 's/^phase \([a-z]*\) [0-9][0-9]*\.[0-9][0-9]*$/\1/p' "$TMP/err.txt" | tr '\n' ' ')
want="lex parse merge typecheck analysis codegen "
if [ "$names" != "$want" ]; then
    echo "  [FAIL] aetherc_time_phases: phases '$names', want '$want'"
    cat "$TMP/err.txt"
    exit 1
fi
if ! same_bytes "$TMP/timed.c" "$TMP/plain.c"; then
    echo "  [FAIL] aetherc_time_phases: the generated C differs with the flag"
    exit 1
fi
echo "  [PASS] aetherc_time_phases"
