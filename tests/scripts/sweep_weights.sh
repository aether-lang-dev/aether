#!/bin/sh
# Make a tests/sweep_weights_<os>.txt, the cost of each unit of the test
# sweep on one system, from the per-test times its CI sweep wrote
# (AE_SWEEP_TIMINGS_OUT, sweep_timings.sh; the legs upload them).
#
#   sweep_weights.sh <timings file>... > tests/sweep_weights_linux.txt
#
# Run from the repository root. Several files are read as one: the halves of
# a split leg each hold part of the sweep, and a test in two of them keeps
# the larger time. A unit is what the sweep hands one worker: a .ae program,
# a test_*.sh of a DRIVERS_INDEPENDENT directory, or a whole
# tests/integration/<dir>, whose drivers run one after another, so its cost
# is the sum of theirs. Each line is "<ms> <unit>", sorted by unit. A unit
# the times do not name (a test added since, or one that skipped) is left
# out, and sweep_shard.sh gives it the median cost.
#
# Why (#3601): the halves of a split leg took the test list by position and
# finished minutes apart (macOS x86_64 13 and 21 to 26 minutes, Windows GCC
# 18.6 and 27.6), the heavy tests falling unevenly between them. Each system
# has a file of its own: a test can cost minutes on one and be skipped on
# another (cross_ios runs on macOS alone).
set -u
[ "$#" -gt 0 ] || { echo "usage: sweep_weights.sh <timings file>..." >&2; exit 2; }
for f in "$@"; do
    [ -s "$f" ] || { echo "sweep_weights.sh: no timings file '$f'" >&2; exit 2; }
done
. "$(dirname "$0")/sweep_name.sh"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# The times, a name once (its largest time).
cat "$@" | awk '{ if (!($2 in t) || $1 > t[$2]) t[$2] = $1 } END { for (n in t) print t[n], n }' > "$tmp/times"
# Every path the sweep runs and its name as the times spell it, named by one
# sed (a process per path costs minutes on Windows).
sh tests/scripts/ae_sweep_list.sh "" > "$tmp/ae"
find tests/integration -name 'test_*.sh' | LC_ALL=C sort > "$tmp/sh"
find tests/integration -name DRIVERS_INDEPENDENT | sed 's|/DRIVERS_INDEPENDENT$||' > "$tmp/indep"
sed "$SWEEP_NAME_SED" "$tmp/ae" | paste -d ' ' "$tmp/ae" - > "$tmp/ae_units"
# A driver belongs to its directory's unit, unless the directory's drivers
# are units of their own.
sed "$SWEEP_NAME_SED" "$tmp/sh" | paste -d ' ' "$tmp/sh" - |
    awk 'NR == FNR { indep[$1] = 1; next }
         { d = $1; sub("/[^/]*$", "", d); print ((d in indep) ? $1 : d), $2 }' "$tmp/indep" - > "$tmp/sh_units"
cat "$tmp/ae_units" "$tmp/sh_units" |
    awk 'NR == FNR { t[$2] = $1; next }
         ($2 in t) { w[$1] += t[$2]; seen[$1] = 1 }
         END { for (u in seen) printf "%d %s\n", w[u], u }' "$tmp/times" - |
    LC_ALL=C sort -k2,2
