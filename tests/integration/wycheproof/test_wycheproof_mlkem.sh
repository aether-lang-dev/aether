#!/bin/sh
# Wycheproof adversarial vector suite: ML-KEM (FIPS 203) decapsulation,
# encapsulation and the input checks (#2482).
#
# ML-KEM-512/-768/-1024 is symmetric-crypto-fast, so this driver sweeps ALL
# ~1400 cases by default in a few seconds (no sampling needed), with no
# separate nightly-full step. WYCHEPROOF_STRIDE=N can thin it if ever needed.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$ROOT" || exit 1

AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"
[ -x "$AE" ] || { echo "  [FAIL] wycheproof_mlkem: build/ae missing (run make)"; exit 1; }

out="$("$AE" run "tests/integration/wycheproof/wp_mlkem.ae" 2>&1)"
if printf '%s' "$out" | grep -q "^ALL PASS"; then
    printf '%s\n' "$out" | grep "^wycheproof" | sed 's/^/  [PASS] /'
    exit 0
fi
echo "  [FAIL] wycheproof mlkem:"
printf '%s\n' "$out" | tail -16 | sed 's/^/        /'
exit 1
