#!/bin/sh
# Wycheproof adversarial vector suites — fast (symmetric + KDF) families.
#
# Runs the cheap drivers against the pinned vector JSONs in
# tests/vectors/wycheproof/ (provenance + licence in the README there).
# Driver semantics: "valid" must succeed with matching output, "invalid"
# must be rejected, "acceptable" may be rejected but must match when
# accepted. AEAD drivers additionally check the seal direction.
#
# The bignum-backed asymmetric families live in sibling scripts
# (test_wycheproof_asym.sh, test_wycheproof_ecdsa.sh) so each stays
# inside the harness's 180s per-test budget; sampling strides and the
# WYCHEPROOF_FULL=1 / WYCHEPROOF_STRIDE=N overrides are documented
# there and in each driver.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$ROOT" || exit 1

AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"
[ -x "$AE" ] || { echo "  [FAIL] wycheproof: build/ae missing (run make)"; exit 1; }

rc=0
for drv in wp_x25519 wp_chacha20poly1305 wp_aes_gcm wp_hmac_hkdf; do
    out="$("$AE" run "tests/integration/wycheproof/$drv.ae" 2>&1)"
    if printf '%s' "$out" | grep -q "^ALL PASS"; then
        # A driver may cover several families (wp_hmac_hkdf) — print every
        # per-family summary line, not just the first.
        printf '%s\n' "$out" | grep "^wycheproof" | sed 's/^/  [PASS] /'
    else
        echo "  [FAIL] wycheproof $drv:"
        printf '%s\n' "$out" | tail -12 | sed 's/^/        /'
        rc=1
    fi
done
exit $rc
