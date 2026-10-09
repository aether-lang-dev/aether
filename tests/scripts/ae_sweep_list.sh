#!/bin/sh
# Print the .ae programs the `make test-ae` sweep builds and runs, one path
# per line, sorted. Run from the repository root.
#
#   ae_sweep_list.sh [extra-prune-file]
#
# The sweep in `test-ae` and the -O0 against -O2 comparison in
# `test-ae-opt-diff` (#2488) both read their list from here, so the two always
# cover the same programs: a copy of the find in each would drift the first
# time one of them changed.
#
# tests/ae_sweep_prune.txt holds the paths left out (fixed substrings), and the
# optional argument names a file of more, as AE_SWEEP_EXTRA_PRUNE does for
# `test-ae`.
set -u

prune="$(mktemp)"
trap 'rm -f "$prune"' EXIT

# Blank lines are dropped too: as a pattern for `grep -F -f`, an empty line
# matches every path, and the list would come out empty.
sed '/^#/d;/^$/d' tests/ae_sweep_prune.txt > "$prune"
if [ -n "${1:-}" ]; then
    sed '/^#/d;/^$/d' "$1" >> "$prune"
fi

{ find tests/syntax tests/compiler tests/integration tests/regression -name '*.ae' -print 2>/dev/null
  find std -name 'test_*.ae' -print 2>/dev/null; } \
    | grep -v -F -f "$prune" | sort
