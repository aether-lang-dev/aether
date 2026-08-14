#!/bin/sh
# Wycheproof adversarial vector suites — RSA / Ed25519 / X448.
#
# Split from test_wycheproof.sh: these route through variable-time
# std.bignum, and the harness kills any single shell test at 180s.
# Budget shape at CI's -O0:
#   rsa v1.5 full 259 cases   (~40-60s: one small-e modexp per case)
#   rsa pss  full 108 cases   (~40s, same op + MGF1)
#   x448     stride 25 of 510 (~40s)
# (ed25519 rides in test_wycheproof.sh; ECDSA has its own slot.)
# WYCHEPROOF_FULL=1 (the nightly) sweeps everything; WYCHEPROOF_STRIDE=N
# picks a custom density.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$ROOT" || exit 1

AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"
[ -x "$AE" ] || { echo "  [FAIL] wycheproof_asym: build/ae missing (run make)"; exit 1; }

rc=0
for drv in wp_rsa_pkcs1 wp_rsa_pss wp_x448; do
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
