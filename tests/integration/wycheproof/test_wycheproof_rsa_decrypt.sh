#!/bin/sh
# Wycheproof adversarial vector suites — RSA decryption families.
#
# Own harness slot (180s per-test budget): each case is a full-size
# private-key modexp (~1-2s at CI's -O0), unlike the cheap public-op
# verify families in test_wycheproof_asym.sh.
#   rsaes-pkcs1 2048   full 67 cases  (Bleichenbacher padding shapes)
#   rsa-oaep 2048      full 37 cases  (Manger shapes; 8 label cases
#                      counted-skipped: decrypt_oaep has no label param)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$ROOT" || exit 1

AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"
[ -x "$AE" ] || { echo "  [FAIL] wycheproof_rsa_decrypt: build/ae missing (run make)"; exit 1; }

rc=0
for drv in wp_rsa_decrypt_pkcs1 wp_rsa_oaep; do
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
