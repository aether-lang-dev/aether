#!/bin/sh
# Print the .ae programs the `make test-ae` sweep builds and runs, one path
# per line, sorted. Run from the repository root.
#
#   ae_sweep_list.sh [extra-prune-file [select-file]]
#
# The sweep in `test-ae` and the -O0 against -O2 comparison in
# `test-ae-opt-diff` (#2488) both read their list from here, so the two always
# cover the same programs: a copy of the find in each would drift the first
# time one of them changed.
#
# tests/ae_sweep_prune.txt holds the paths left out (fixed substrings), and the
# optional argument names a file of more, as AE_SWEEP_EXTRA_PRUNE does for
# `test-ae`. A select file, as AE_SWEEP_SELECT names it, keeps only the
# programs it picks (tests/scripts/sweep_select.sh; `make test-tsan`, #3542).
set -u

prune="$(mktemp)"
trap 'rm -f "$prune"' EXIT

# Blank lines are dropped too: as a pattern for `grep -F -f`, an empty line
# matches every path, and the list would come out empty.
sed '/^#/d;/^$/d' tests/ae_sweep_prune.txt > "$prune"
if [ -n "${1:-}" ]; then
    sed '/^#/d;/^$/d' "$1" >> "$prune"
fi

# A select file the selector refuses stops the list here: inside the pipe
# below its status is lost, and the list would come out empty and pass.
if [ -n "${2:-}" ]; then
    sh "$(dirname "$0")/sweep_select.sh" "$2" < /dev/null > /dev/null || exit 2
fi

{ find tests/syntax tests/compiler tests/integration tests/regression -name '*.ae' -print 2>/dev/null
  find std -name 'test_*.ae' -print 2>/dev/null; } \
    | grep -v -F -f "$prune" | sh "$(dirname "$0")/sweep_select.sh" "${2:-}" | sort
