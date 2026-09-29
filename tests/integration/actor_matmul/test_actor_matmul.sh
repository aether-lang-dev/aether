#!/bin/sh
# #1986: every matrix cell, uneven partitions, empty partitions, and
# repeated use of the same worker pool and borrowed buffers.
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT HUP INT TERM
export AETHER_HOME="$ROOT"
"$ROOT/build/ae" build "$ROOT/examples/actors/matmul.ae" -o "$work/matmul"
for workers in 1 4 8 16 47; do
    "$work/matmul" 31 "$workers" 3 > "$work/result"
    grep -q '^PASS: every output cell matches the serial reference$' "$work/result"
    test "$(grep -c '^31,' "$work/result")" = 3
done
"$work/matmul" 1 16 2 > "$work/result"
grep -q '^PASS: every output cell matches the serial reference$' "$work/result"
for args in '0 4 1' '7 0 1' '7 4 0' '4097 4 1' '7 257 1' 'no 4 1'; do
    if "$work/matmul" $args > "$work/result" 2>&1; then
        echo "FAIL: invalid arguments accepted: $args"
        exit 1
    fi
done
echo "PASS: actor matmul partitions, buffer reuse, and argument validation"
