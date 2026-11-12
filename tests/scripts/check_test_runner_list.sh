#!/bin/sh
# Every test the C unit runner's sources register by hand is registered in
# every mode of the runner. Run by `make test` after the suite.
#
#   check_test_runner_list.sh <test_runner> <source.c>...
#
# The runner's TEST() tests register themselves before main; the others are
# registered by the register_*_tests() functions main calls. main once called
# those only on its way to the full run, after the --category=NAME branch had
# returned, so `test_runner --category=runtime` ran 110 of the 180 runtime
# tests (MinGW), none of the scheduler, mailbox or actor suites, and
# passed. Now main registers everything first; this asks the runner, through
# its --list mode (which returns as early as --category does), for the tests
# it knows, and fails on any name a source passes to
# register_test_with_category() that the list does not hold.
set -u
# One collation for sort and comm.
LC_ALL=C
export LC_ALL

runner="$1"
shift
tmp=$(mktemp -d) || exit 2
trap 'rm -rf "$tmp"' EXIT

if ! "$runner" --list > "$tmp/list" 2> "$tmp/err"; then
    echo "check_test_runner_list.sh: $runner --list failed:" >&2
    sed 's/^/  | /' "$tmp/err" >&2
    exit 1
fi
# "<category>\t<name>"; the name alone, without a Windows console's CR.
tr -d '\r' < "$tmp/list" | cut -f2- | sort -u > "$tmp/listed"
# The names passed to register_test_with_category() outside any #if (a
# registration a platform leaves out is not one the runner must list) and
# outside a // comment.
awk 'FNR == 1 { depth = 0 }
     /^[ \t]*#[ \t]*if/ { depth++ }
     /^[ \t]*#[ \t]*endif/ { depth-- }
     depth == 0 && !/^[ \t]*\/\// && match($0, /register_test_with_category\("[^"]*"/) {
         print substr($0, RSTART + 29, RLENGTH - 30)
     }' "$@" | sort -u > "$tmp/by_hand"
if [ ! -s "$tmp/by_hand" ]; then
    echo "check_test_runner_list.sh: no register_test_with_category(\"...\") call in the sources given" >&2
    exit 1
fi
comm -23 "$tmp/by_hand" "$tmp/listed" > "$tmp/missing"
if [ -s "$tmp/missing" ]; then
    echo "check_test_runner_list.sh: $(wc -l < "$tmp/missing" | tr -d ' ') test(s) registered by hand are not registered when the runner lists its tests:" >&2
    sed 's/^/  /' "$tmp/missing" >&2
    exit 1
fi
echo "  [PASS] the runner registers all $(wc -l < "$tmp/by_hand" | tr -d ' ') tests its sources register by hand, in every mode"
