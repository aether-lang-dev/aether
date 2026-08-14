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
# wp_x448 samples its 510 cases at stride 25 by default (~20 cases): its
# field math is bignum-based at ~1-2s/agreement, so the full sweep costs
# ~10 minutes and must fit the test harness's 180s per-test budget next
# to the three fast drivers. Set WYCHEPROOF_FULL=1 (the nightly does)
# for everything; WYCHEPROOF_STRIDE=N picks a custom sample density.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$ROOT" || exit 1

AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"
[ -x "$AE" ] || { echo "  [FAIL] wycheproof: build/ae missing (run make)"; exit 1; }

rc=0
for drv in wp_x25519 wp_chacha20poly1305 wp_aes_gcm wp_x448; do
    out="$("$AE" run "tests/integration/wycheproof/$drv.ae" 2>&1)"
    if printf '%s' "$out" | grep -q "^ALL PASS"; then
        summary="$(printf '%s' "$out" | grep "^wycheproof" | head -1)"
        echo "  [PASS] $summary"
    else
        echo "  [FAIL] wycheproof $drv:"
        printf '%s\n' "$out" | tail -12 | sed 's/^/        /'
        rc=1
    fi
done
exit $rc
