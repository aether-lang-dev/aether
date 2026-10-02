#!/bin/sh
# #2340: an uncaught panic prints a message built at run time.
#
# A heap-built message reaches the runtime as the AetherString codegen made
# it (the catch lowering adopts that object), and the no-frame fallback
# printed it with %s, so the line showed the string's header bytes
# (DE C0 57 AE ...) instead of the text. A literal printed fine.
#
# Asserts the uncaught line carries the text for an interpolated message, a
# heap-tracked local, and (unchanged) a literal.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] panic_uncaught_message: ae not built"; exit 0; }

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

expect_line() {
    # $1: label, $2: main body, $3: expected message text
    printf 'main() {\n    index = 5\n    buflen = 4\n%s\n}\n' "$2" > "$T/p.ae"
    if ! "$AE" build "$T/p.ae" -o "$T/p" >"$T/build.log" 2>&1; then
        echo "  [FAIL] panic_uncaught_message: $1: build failed"
        sed 's/^/    /' "$T/build.log" | head -10
        exit 1
    fi
    OUT=$(AETHER_STACK_TRACE=0 "$T/p" 2>&1)
    case "$OUT" in
        *"panic outside any try/catch or actor: $3"*) ;;
        *) echo "  [FAIL] panic_uncaught_message: $1: the line does not carry the message"
           printf '%s\n' "$OUT" | od -c | head -6 | sed 's/^/    /'
           exit 1 ;;
    esac
}

expect_line "interpolated" \
    '    panic("test: ${index} is outside a slice of ${buflen}")' \
    "test: 5 is outside a slice of 4"
expect_line "heap local" \
    '    msg = "local ${index}"
    panic(msg)' \
    "local 5"
expect_line "literal" \
    '    panic("plain")' \
    "plain"

echo "  [PASS] panic_uncaught_message: uncaught panics print built and literal messages"
