#!/bin/sh
# string.release on a tracked local holding an untracked value still releases
# it (#1977).
#
# Asserted on the GENERATED C rather than by leak-counting a running binary:
# the defect is that one branch was never emitted, so the emitted code is the
# precise statement of the fix and it checks the same on every platform.
# The leak gates cover the runtime side.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
AE="$ROOT/build/ae"
[ -x "$AETHERC" ] || { echo "  [SKIP] string_release_untracked: build/aetherc missing"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if ! "$AETHERC" "$SCRIPT_DIR/probe.ae" "$TMP/out.c" > "$TMP/gen.log" 2>&1; then
    echo "  [FAIL] string_release_untracked: codegen failed"
    sed 's/^/        /' "$TMP/gen.log" | head -10
    exit 1
fi

# The guarded free must have a fallback. Without it the release is a no-op
# whenever the flag is 0, which is the whole bug.
if ! grep -q "else { string_release(prev); }" "$TMP/out.c"; then
    echo "  [FAIL] string_release_untracked: the release has no fallback for an"
    echo "         untracked value, so it does nothing when _heap_prev is 0"
    grep -n "_heap_prev" "$TMP/out.c" | sed 's/^/        /' | head -5
    exit 1
fi

# And it must still free through the flag when the value IS tracked, so the
# fix did not simply replace one path with the other.
grep -q "if (_heap_prev) { aether_heap_str_free" "$TMP/out.c" || {
    echo "  [FAIL] string_release_untracked: the tracked-value free was lost"
    exit 1
}

# The program still builds and runs.
if [ -x "$AE" ]; then
    if "$AE" build "$SCRIPT_DIR/probe.ae" -o "$TMP/probe" > "$TMP/build.log" 2>&1; then
        if "$TMP/probe" > "$TMP/run.out" 2>&1; then
            grep -q "^done: value 199$" "$TMP/run.out" || {
                echo "  [FAIL] string_release_untracked: wrong output"
                sed 's/^/        /' "$TMP/run.out" | head -5
                exit 1
            }
        fi
    fi
fi

echo "  [PASS] string_release_untracked: release frees an untracked value too"
exit 0
