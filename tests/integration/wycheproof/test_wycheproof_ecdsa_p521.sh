#!/bin/sh
# Wycheproof adversarial vector suites — ECDSA P-521 (P1363 + DER forms), wave 5.
#
# Its own harness slot. A P-521 verify is two 521-bit scalar multiplications,
# which took seconds each while std.bignum divided a bit at a time, so the
# drivers sampled every 40th vector. With Algorithm D (#2595) one takes about
# 60 ms; the default stride is now 10, as for P-256 and P-384, about 55 (DER)
# + 32 (P1363) cases. WYCHEPROOF_FULL=1 (the nightly) sweeps all 860,
# WYCHEPROOF_STRIDE=N picks a custom density.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$ROOT" || exit 1

AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"
[ -x "$AE" ] || { echo "  [FAIL] wycheproof_ecdsa_p521: build/ae missing (run make)"; exit 1; }

rc=0
for drv in wp_ecdsa_p521 wp_ecdsa_p521_der; do
    out="$("$AE" run "tests/integration/wycheproof/$drv.ae" 2>&1)"
    if printf '%s' "$out" | grep -q "^ALL PASS"; then
        printf '%s\n' "$out" | grep "^wycheproof" | sed 's/^/  [PASS] /'
    else
        echo "  [FAIL] wycheproof $drv:"
        printf '%s\n' "$out" | tail -12 | sed 's/^/        /'
        rc=1
    fi
done
exit $rc
