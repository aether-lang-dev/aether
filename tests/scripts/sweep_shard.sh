#!/bin/sh
# Keeps the lines of stdin that belong to shard k of n of the test sweep,
# AE_SWEEP_SHARD=k/n, so the n shards of one list cover it once between
# them: by cost where the weights below are known, else the lines whose
# 0-based position modulo n is k-1. k can be a comma-separated list
# (AE_SWEEP_SHARD=2,3/3 keeps shards 2 and 3). Without AE_SWEEP_SHARD every
# line passes.
#
# Why (#2757): the long CI legs (Windows MSYS2 x2, macOS x86_64) spend most
# of their 23 minutes in the sweep, on 4 cores. Each is split into two jobs
# that run in parallel. On Windows one runs `make ci` with 7 of 20 shares
# (AE_SWEEP_SHARD=1,2,...,7/20) and the other `make ci-sweep` (the toolchain
# build and the sweep alone) with the other 13: the first also runs every
# stage of `make ci` around the sweep, about 8.6 minutes more on those runners,
# so it takes the smaller share and the two finish together (#3601; by
# thirds, before the split by cost, they finished 4 to 9 minutes apart). On
# macOS x86_64 both run `make ci-sweep`, with 1/2 and 2/2, since the ARM64
# leg runs the rest of `make ci` on macOS (#2772).

shard="${AE_SWEEP_SHARD:-}"
[ -n "$shard" ] || exec cat

usage() {
    echo "sweep_shard.sh: AE_SWEEP_SHARD must be k/n or k,k.../n with 1 <= k <= n, got '$shard'" >&2
    exit 2
}

case "$shard" in
    */*) ;;
    *) usage ;;
esac
ks=${shard%/*}
n=${shard##*/}
case "$n" in ''|*[!0-9]*) usage ;; esac
[ "$n" -ge 1 ] || usage
case "$ks" in ''|,*|*,|*,,*|*[!0-9,]*) usage ;; esac
for k in $(printf '%s\n' "$ks" | tr ',' ' '); do
    [ "$k" -ge 1 ] && [ "$k" -le "$n" ] || usage
done
# By cost, where the sweep's weights are known (#3601): the file
# tests/sweep_weights_<os>.txt (linux, macos, windows) holds "<ms> <unit>"
# from that system's CI sweep (sweep_weights.sh), since a test can cost
# minutes on one and nothing on another (cross_ios). The units go out
# heaviest first, each to the shard whose total is lowest so far, so the
# shards' totals come out close together; a unit the file does not name costs
# the median of those it does. Every job splits the same list the same way
# (ties go by position), and each shard's units come out heaviest first, which
# also lets its workers start the long ones early. AE_SWEEP_WEIGHTS names
# another file; set and empty, it splits by position, as without the file.
case "$(uname -s 2>/dev/null)" in
    Darwin) os=macos ;;
    MINGW*|MSYS*|CYGWIN*) os=windows ;;
    *) os=linux ;;
esac
weights="${AE_SWEEP_WEIGHTS-$(dirname "$0")/../sweep_weights_$os.txt}"
if [ -z "$weights" ] || [ ! -s "$weights" ]; then
    awk -v ks="$ks" -v n="$n" '
        BEGIN { m = split(ks, list, ","); for (i = 1; i <= m; i++) keep[list[i] - 1] = 1 }
        ((NR - 1) % n) in keep
    '
    exit 0
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
tab=$(printf '\t')
awk -v OFS="$tab" 'NR == FNR { w[$2] = $1; next }
                   { print (($0 in w) ? w[$0] : -1), FNR, $0 }' "$weights" - > "$tmp/costed"
median=$(awk -F"$tab" '$1 >= 0 { print $1 }' "$tmp/costed" | LC_ALL=C sort -n |
         awk '{ a[NR] = $1 } END { print NR ? a[int((NR + 1) / 2)] : 1 }')
awk -F"$tab" -v OFS="$tab" -v m="$median" '{ if ($1 < 0) $1 = m; print }' "$tmp/costed" |
    LC_ALL=C sort -t "$tab" -k1,1nr -k2,2n |
    awk -F"$tab" -v ks="$ks" -v n="$n" '
        BEGIN {
            m = split(ks, list, ",")
            for (i = 1; i <= m; i++) keep[list[i] - 1] = 1
            for (b = 0; b < n; b++) load[b] = 0
        }
        {
            best = 0
            for (b = 1; b < n; b++) if (load[b] < load[best]) best = b
            load[best] += $1
            if (best in keep) print $3
        }'
