#!/bin/sh
# asn1.encode_oid refuses non-OIDs (#1947) and the X.509 TBSCertificate parse
# refuses a certificate whose fields it could not read (#1942).
#
# Both bugs were an available error going unread, so the caller received a
# confident answer assembled from a failed read: DER that decodes as a
# different identifier than the caller named, and a LeafCert whose issuer,
# subject, validity or SPKI came from reads that did not succeed.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] asn1_oid_and_cert_validation: build/ae missing"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if ! AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/probe.ae" -o "$TMP/probe" > "$TMP/build.log" 2>&1; then
    echo "  [FAIL] asn1_oid_and_cert_validation: build failed"
    sed 's/^/        /' "$TMP/build.log" | head -15
    exit 1
fi

if ! "$TMP/probe" > "$TMP/run.out" 2>&1; then
    echo "  [FAIL] asn1_oid_and_cert_validation: probe reported failures"
    sed 's/^/        /' "$TMP/run.out" | head -20
    exit 1
fi

grep -q "All OID and certificate validation cases pass" "$TMP/run.out" || {
    echo "  [FAIL] asn1_oid_and_cert_validation: probe did not reach the end"
    sed 's/^/        /' "$TMP/run.out" | head -20
    exit 1
}

echo "  [PASS] asn1_oid_and_cert_validation: non-OIDs and unreadable certificates are refused"
exit 0
