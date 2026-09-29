#!/bin/sh
# print flushes stdout, so a partial line shows at once and survives a
# process that ends without the flush at exit (killed by a timeout, _exit).
# An interpolated print skipped that flush: a spec runner killed by its
# timeout lost the line reporting the last spec that passed, and the log
# pointed at the wrong spec as the one that hung.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] print_interp_flush: $AE not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

if ! AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/partial.ae" -o "$TMPDIR/partial" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] print_interp_flush: build failed"
    head -5 "$TMPDIR/build.log" | sed 's/^/        /'
    exit 1
fi

# Into a file, where stdout is fully buffered.
"$TMPDIR/partial" >"$TMPDIR/out.txt" 2>&1
got="$(cat "$TMPDIR/out.txt")"
want='literal, interpolated 42'
if [ "$got" = "$want" ]; then
    echo "  [PASS] print_interp_flush: interpolated print flushes like a literal one"
    exit 0
fi
echo "  [FAIL] print_interp_flush: stdout was [$got], expected [$want]"
exit 1
