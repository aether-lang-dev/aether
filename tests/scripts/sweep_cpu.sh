#!/bin/sh
# How busy the machine's cores were during each phase of the test sweep.
#
#   sweep_cpu.sh mark <tmpdir> <label>   record the time and the CPU counters
#   sweep_cpu.sh report <tmpdir> <tests at a time> <shell tests at a time>
#                                        print each phase's wall time and use
#
# The sweep runs <nproc> tests at a time, and its timings (sweep_timings.sh)
# say how long each test held its place. They cannot say whether the cores
# were working: a test that waits on a server, a timeout or a sleep holds a
# place and leaves its core idle. When the cores are mostly busy, a faster
# sweep needs less work; when they are not, it needs more tests at a time.
# This tells the two apart, per platform, from the run's own log.
#
# The counters are /proc/stat's first line (Linux, and MSYS2 on Windows,
# whose runtime provides it): busy is every field but idle and iowait. A
# system without /proc/stat (macOS, FreeBSD) gets the wall times, and the
# whole sweep's CPU from the `times` of the shell that ran it.
set -u
cmd="${1:-}"
tmpdir="${2:-}"
[ -n "$cmd" ] && [ -d "$tmpdir" ] || exit 0

counters() {
    [ -r /proc/stat ] || return 0
    awk 'NR == 1 && $1 == "cpu" {
        busy = 0; total = 0
        for (i = 2; i <= NF; i++) { total += $i; if (i != 5 && i != 6) busy += $i }
        printf "%.0f %.0f", busy, total
        exit
    }' /proc/stat
}

case "$cmd" in
    mark)
        label="${3:-}"
        [ -n "$label" ] || exit 0
        printf '%s %s\n' "$(date +%s)" "$(counters)" > "$tmpdir/cpu_$label.txt"
        ;;
    report)
        ae_n="${3:-1}"
        sh_n="${4:-$ae_n}"
        line() { # label from-mark to-mark tests-at-a-time
            [ -s "$tmpdir/cpu_$2.txt" ] && [ -s "$tmpdir/cpu_$3.txt" ] || return 0
            cat "$tmpdir/cpu_$2.txt" "$tmpdir/cpu_$3.txt" | awk -v label="$1" -v n="$4" '
                NR == 1 { t0 = $1; b0 = $2; a0 = $3 }
                NR == 2 { t1 = $1; b1 = $2; a1 = $3 }
                END {
                    wall = t1 - t0
                    if (a1 > a0)
                        printf "  %-13s %5d s wall, cores busy %3.0f%% (%d at a time)\n",
                               label, wall, 100 * (b1 - b0) / (a1 - a0), n
                    else
                        printf "  %-13s %5d s wall (%d at a time; no CPU counters here)\n",
                               label, wall, n
                }'
        }
        echo "  CPU use by phase (sweep_cpu.sh):"
        line ".ae tests" ae_start ae_end "$ae_n"
        line "shell tests" sh_start sh_end "$sh_n"
        # The recipe's shell waited for every test, so the CPU its children
        # used (`times`, second line: user and system) is the sweep's, on any
        # system: the one figure macOS gets.
        [ -s "$tmpdir/times.txt" ] && [ -s "$tmpdir/cpu_ae_start.txt" ] &&
            [ -s "$tmpdir/cpu_sh_end.txt" ] || exit 0
        t0=$(cut -d' ' -f1 "$tmpdir/cpu_ae_start.txt")
        t1=$(cut -d' ' -f1 "$tmpdir/cpu_sh_end.txt")
        awk -v wall=$((t1 - t0)) -v n="$ae_n" '
            function secs(tok,   m, s) {
                m = tok; sub(/m.*/, "", m)
                s = tok; sub(/^[0-9]+m/, "", s); sub(/s$/, "", s)
                return m * 60 + s
            }
            NR == 2 { cpu = secs($1) + secs($2) }
            END {
                if (wall > 0)
                    printf "  %-13s %5d s wall, %d s of CPU in the tests: %3.0f%% of %d cores\n",
                           "whole sweep", wall, cpu, 100 * cpu / (wall * n), n
            }' "$tmpdir/times.txt"
        ;;
esac
exit 0
