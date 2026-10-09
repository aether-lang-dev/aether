#!/bin/sh
# Wycheproof adversarial vector suites — Ed448 signature verify + AES-CMAC, wave 6.
#
# Two families sharing one harness slot, both swept in full by default:
# AES-CMAC is symmetric-fast, and Ed448's 87 verifies take about 2 s since
# std.bignum's division is Algorithm D (#2595; at seconds each it sampled every
# 4th). WYCHEPROOF_STRIDE=N samples both.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$ROOT" || exit 1

AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"
[ -x "$AE" ] || { echo "  [FAIL] wycheproof_ed448_cmac: build/ae missing (run make)"; exit 1; }

rc=0
for drv in wp_aes_cmac wp_ed448; do
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
