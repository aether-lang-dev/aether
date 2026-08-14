#!/bin/sh
# Wycheproof adversarial vector suites (#739 / #1298).
#
# Runs each per-family driver in this directory against the pinned vector
# JSONs in tests/vectors/wycheproof/ (provenance + licence in the README
# there). The drivers encode Wycheproof semantics: "valid" must succeed
# with matching output, "invalid" must be rejected, "acceptable" may be
# rejected but must match when accepted. AEAD drivers additionally check
# the seal direction (deterministic AEADs).
#
# wp_x448 (stride 5 of 510) and wp_ecdsa_p256 (stride 5 of 262) sample
# by default: their field math is bignum-based at ~1s+/op, so the full
# sweeps cost ~10 minutes each. Set WYCHEPROOF_FULL=1 (the nightly does)
# for everything; WYCHEPROOF_STRIDE=N picks a custom sample density.
# ed25519 (151) and rsa (259) run complete — each caught a real
# accepted-forgery bug on import (see CHANGELOG), so no sampling there.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$ROOT" || exit 1

AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"
[ -x "$AE" ] || { echo "  [FAIL] wycheproof: build/ae missing (run make)"; exit 1; }

rc=0
for drv in wp_x25519 wp_chacha20poly1305 wp_aes_gcm wp_hmac_hkdf wp_rsa_pkcs1 wp_ed25519 wp_ecdsa_p256 wp_x448; do
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
