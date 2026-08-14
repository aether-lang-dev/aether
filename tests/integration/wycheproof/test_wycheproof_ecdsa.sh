#!/bin/sh
# Wycheproof adversarial vector suites — ECDSA P-256 (P1363 form).
#
# Its own harness slot: an ECDSA verify is two bignum scalar
# multiplications (~2-4s each at CI's -O0), so even modest sampling
# needs most of the 180s per-test budget. Default stride 10 (~26
# cases); WYCHEPROOF_FULL=1 (the nightly) sweeps all 262,
# WYCHEPROOF_STRIDE=N picks a custom density.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$ROOT" || exit 1

AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"
[ -x "$AE" ] || { echo "  [FAIL] wycheproof_ecdsa: build/ae missing (run make)"; exit 1; }

out="$("$AE" run "tests/integration/wycheproof/wp_ecdsa_p256.ae" 2>&1)"
if printf '%s' "$out" | grep -q "^ALL PASS"; then
    printf '%s\n' "$out" | grep "^wycheproof" | sed 's/^/  [PASS] /'
    exit 0
fi
echo "  [FAIL] wycheproof wp_ecdsa_p256:"
printf '%s\n' "$out" | tail -12 | sed 's/^/        /'
exit 1
