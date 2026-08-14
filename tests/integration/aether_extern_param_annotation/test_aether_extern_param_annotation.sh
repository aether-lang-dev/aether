#!/bin/sh
# Regression: per-param `@aether` annotation suppresses the call-site
# aether_string_data() unwrap on a per-arg-slot basis. See #351.
#
# Three cases locked in:
#  1. Positive: `consume_binary(s: @aether string)` — call site emits
#     consume_binary(s) with no unwrap; round-trips binary content.
#  2. Negative: bare `extern foo(s: string)` still emits
#     aether_string_data() unwrap (#297 unchanged).
#  3. Mixed-discrimination: a single extern with one `@aether string`
#     and one bare `string` param emits the unwrap on the bare param
#     only, preserving the @aether one. This is the expressive
#     advantage of per-param over the whole-function form.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] aether_extern_param_annotation: toolchain not built"
    exit 0
fi

PREFIX="$HOME/.local"
if [ ! -f "$PREFIX/lib/aether/libaether.a" ] || \
   [ ! -d "$PREFIX/include/aether" ]; then
    echo "  [SKIP] aether_extern_param_annotation: no install at $PREFIX (run 'make install PREFIX=\$HOME/.local')"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

# === Case 1: positive — @aether on param, header round-trips ===
cat > "$tmpdir/helper.ae" <<'AE'
import std.string
import std.bytes

export make_binary() -> string {
    b = bytes.new(16)
    // Set every byte 0..6 explicitly. bytes.finish(b, 7) tracks the
    // high-water mark of writes, not the cap argument, so leaving
    // bytes uninitialized produces a shorter-than-expected string.
    bytes.set(b, 0, 83)
    bytes.set(b, 1, 86)
    bytes.set(b, 2, 78)
    bytes.set(b, 3, 1)
    bytes.set(b, 4, 0)
    bytes.set(b, 5, 14)
    bytes.set(b, 6, 14)
    return bytes.finish(b, 7)
}

export consume_binary(s: string) {
    println("len=${string.length(s)} byte5=${string.char_at(s, 5)}")
}

main() { println("library mode") }
AE

cat > "$tmpdir/positive.ae" <<'AE'
import std.string

extern make_binary() -> string
extern consume_binary(s: @aether string)

main() {
    s = make_binary()
    consume_binary(s)
}
AE

"$AETHERC" --emit=lib "$tmpdir/helper.ae" "$tmpdir/helper.c" >/dev/null 2>&1 || {
    echo "  [FAIL] helper.ae did not compile"; exit 1
}
"$AETHERC" "$tmpdir/positive.ae" "$tmpdir/positive.c" >/dev/null 2>&1 || {
    echo "  [FAIL] positive.ae (with @aether param) did not compile"; exit 1
}

# Static check: no aether_string_data unwrap at the consume_binary call.
if grep -E "consume_binary\(aether_string_data" "$tmpdir/positive.c" >/dev/null; then
    echo "  [FAIL] @aether param still emits aether_string_data() unwrap"
    grep -n "consume_binary" "$tmpdir/positive.c"
    exit 1
fi

# Runtime check.
INC=$(find "$PREFIX/include/aether" -type d 2>/dev/null | sed 's|^|-I|' | tr '\n' ' ')
gcc -O2 -Wall -Wextra -Werror $INC \
    "$tmpdir/positive.c" "$tmpdir/helper.c" \
    -L"$PREFIX/lib/aether" -laether \
    -pthread -lm -ldl \
    -o "$tmpdir/positive" >/dev/null 2>"$tmpdir/link.err" || {
    echo "  [FAIL] positive case link failed"
    head -3 "$tmpdir/link.err"
    exit 1
}
out=$("$tmpdir/positive" 2>&1)
if ! echo "$out" | grep -q "len=7 byte5=14"; then
    echo "  [FAIL] @aether param did not round-trip binary content"
    echo "    expected: len=7 byte5=14"
    echo "    got:      $out"
    exit 1
fi

# === Case 2: negative — bare extern still strips ===
cat > "$tmpdir/negative.ae" <<'AE'
import std.string

extern make_binary() -> string
extern consume_binary(s: string)

main() {
    s = make_binary()
    consume_binary(s)
}
AE
"$AETHERC" "$tmpdir/negative.ae" "$tmpdir/negative.c" >/dev/null 2>&1 || {
    echo "  [FAIL] negative.ae did not compile"; exit 1
}
if ! grep -E "consume_binary\(aether_string_data" "$tmpdir/negative.c" >/dev/null; then
    echo "  [FAIL] bare extern lost its aether_string_data() unwrap (regressed #297)"
    grep -n "consume_binary" "$tmpdir/negative.c"
    exit 1
fi

# === Case 3: mixed discrimination — first param @aether, second bare ===
cat > "$tmpdir/mixed.ae" <<'AE'
import std.string

extern do_mixed(aether_msg: @aether string, c_path: string) -> int

main() {
    msg = "hello"
    path = "/tmp/foo"
    rc = do_mixed(msg, path)
    println("rc=${rc}")
}
AE
"$AETHERC" "$tmpdir/mixed.ae" "$tmpdir/mixed.c" >/dev/null 2>&1 || {
    echo "  [FAIL] mixed.ae did not compile"; exit 1
}

# The call site must look exactly like:
#   do_mixed(msg, aether_string_data(path))
# Aether arg passed verbatim, c_path arg unwrapped.
if ! grep -qE "do_mixed\(msg, aether_string_data\(path\)\)" "$tmpdir/mixed.c"; then
    echo "  [FAIL] mixed-param discrimination broken"
    echo "    expected: do_mixed(msg, aether_string_data(path))"
    echo "    got:"
    grep -n "do_mixed(" "$tmpdir/mixed.c"
    exit 1
fi

echo "  [PASS] aether_extern_param_annotation"
exit 0
