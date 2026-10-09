#!/bin/sh
# Report the per-test timings the sweep's runners recorded (#2673):
#
#   sweep_timings.sh <tmpdir>
#
# run_ae_test.sh writes time_<name>.txt as "<build ms> <run ms> <name>" and
# run_ae_sh_dir.sh writes shtime_<name>.txt as "<ms> <name>". Prints the sums
# and the slowest tests of each kind, so a test or a toolchain step that got
# slower shows in the log of the run that made it slower, not only in the
# job's total. The sums add up wall time across the parallel jobs, so they
# measure work, not the sweep's elapsed time.
set -u
tmpdir="$1"
top="${AE_SWEEP_TIMING_TOP:-10}"

ae_times="$tmpdir/.timings_ae"
sh_times="$tmpdir/.timings_sh"
cat "$tmpdir"/time_*.txt > "$ae_times" 2>/dev/null
cat "$tmpdir"/shtime_*.txt > "$sh_times" 2>/dev/null
[ -s "$ae_times" ] || [ -s "$sh_times" ] || exit 0

echo ""
echo "=== Sweep timings (summed over parallel jobs) ==="
if [ -s "$ae_times" ]; then
    awk '{ b += $1; r += $2; n++ }
         END { printf "  .ae tests: %d, build %.1f s, run %.1f s, mean build %.0f ms\n",
                      n, b / 1000, r / 1000, n ? b / n : 0 }' "$ae_times"
    echo "  Slowest .ae builds:"
    sort -rn -k1,1 "$ae_times" | head -n "$top" | awk '{ printf "    %7.1f s  %s\n", $1 / 1000, $3 }'
    echo "  Slowest .ae runs:"
    sort -rn -k2,2 "$ae_times" | head -n "$top" | awk '{ printf "    %7.1f s  %s\n", $2 / 1000, $3 }'
fi
if [ -s "$sh_times" ]; then
    awk '{ t += $1; n++ }
         END { printf "  shell tests: %d, %.1f s\n", n, t / 1000 }' "$sh_times"
    echo "  Slowest shell tests:"
    sort -rn -k1,1 "$sh_times" | head -n "$top" | awk '{ printf "    %7.1f s  %s\n", $1 / 1000, $2 }'
fi
rm -f "$ae_times" "$sh_times"
