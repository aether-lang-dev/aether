#!/bin/sh
# Issue #927: multiple ${duration} interpolations in ONE string each
# rendered the SAME value, because _aether_duration_repr() returned a
# shared static buffer — every result in a single printf/snprintf
# pointed at the last-formatted value. A ring of eight buffers fixed it
# up to eight; since #3526 the formatter writes each duration's text
# itself (%D), so any number coexist in one expression.
#
# This test puts three durations of different magnitudes into one
# interpolated string (twice, in different orders) and asserts each
# slot renders its OWN value.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
# A directory of its own, not fixed /tmp paths: two sweeps at once (two
# checkouts, or parallel agents) overwrote each other's files (#2770).
AE_TMP="$(mktemp -d)"
trap 'rm -rf "$AE_TMP"' EXIT

cat > $AE_TMP/ae_dur_interp_multi.ae << 'EOF'
main() {
    a = 5s
    b = 250ms
    c = 3h
    println("a=${a} b=${b} c=${c}")
    println("again ${c} ${a} ${b}")
    // Four in one line, all distinct.
    println("${a} ${b} ${c} ${a}")
}
EOF

output=$(AETHER_HOME="" "$ROOT/build/ae" run $AE_TMP/ae_dur_interp_multi.ae 2>&1)
rc=$?
rm -f $AE_TMP/ae_dur_interp_multi.ae

if [ "$rc" -ne 0 ]; then
    echo "duration_interp_multi: FAIL (program errored, rc=$rc)"
    echo "$output" | head -20 | sed 's/^/  /'
    exit 1
fi

expected_l1="a=5s b=250ms c=3h"
expected_l2="again 3h 5s 250ms"
expected_l3="5s 250ms 3h 5s"

l1=$(echo "$output" | sed -n '1p')
l2=$(echo "$output" | sed -n '2p')
l3=$(echo "$output" | sed -n '3p')

fail=0
[ "$l1" = "$expected_l1" ] || { echo "  line1: got [$l1] want [$expected_l1]"; fail=1; }
[ "$l2" = "$expected_l2" ] || { echo "  line2: got [$l2] want [$expected_l2]"; fail=1; }
[ "$l3" = "$expected_l3" ] || { echo "  line3: got [$l3] want [$expected_l3]"; fail=1; }

if [ "$fail" -ne 0 ]; then
    echo "duration_interp_multi: FAIL (a ${duration} slot rendered the wrong value)"
    exit 1
fi

echo "duration_interp_multi: PASS"
exit 0
