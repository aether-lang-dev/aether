#!/bin/sh
# std.cryptography.rsa OAEP with SHA-1 and MGF1-SHA1, both ways against
# OpenSSL (RSA_PKCS1_OAEP_PADDING, `pkeyutl -pkeyopt rsa_oaep_md:sha1`).
#
# MySQL's caching_sha2_password and sha256_password send the password
# XORed with the scramble, RSA-OAEP encrypted under the server's public key
# with OpenSSL's default OAEP, SHA-1 with MGF1-SHA1. encrypt_oaep was
# SHA-256 only, so a client in Aether could not log in over a plain
# connection (testcontainers-ae's MySQL driver).
#
#   1. A 2048-bit key from openssl genrsa; openssl encrypts a message with
#      OAEP-SHA1, decrypt_oaep_hash(k, "sha1", ct) opens it.
#   2. encrypt_oaep_hash(k, "sha1", msg, random seed); openssl decrypts it.
#
# Skipped without openssl.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/same_bytes.sh"
AE="$ROOT/build/ae"
NAME=crypto_rsa_oaep_sha1_openssl

[ -x "$AE" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }
command -v openssl >/dev/null 2>&1 || { echo "  [SKIP] $NAME: no openssl"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || :' EXIT
fail() { echo "  [FAIL] $NAME: $1"; exit 1; }
ssl() { "$@" >"$TMP/ssl.log" 2>&1 || { sed -n '1,10p' "$TMP/ssl.log"; fail "openssl: $*"; }; }
OAEP1="-pkeyopt rsa_padding_mode:oaep -pkeyopt rsa_oaep_md:sha1 -pkeyopt rsa_mgf1_md:sha1"

ssl openssl genrsa -out "$TMP/k.pem" 2048
# PKCS#1 DER (RSAPrivateKey), what rsa.key_from_pkcs1_der reads.
ssl openssl rsa -in "$TMP/k.pem" -traditional -outform DER -out "$TMP/k.der"
printf 'scramble XOR password\000' >"$TMP/m.bin"
# shellcheck disable=SC2086
ssl openssl pkeyutl -encrypt -inkey "$TMP/k.pem" -in "$TMP/m.bin" -out "$TMP/c.bin" $OAEP1

"$AE" build "$SCRIPT_DIR/oaep_sha1.ae" -o "$TMP/oaep_sha1" >"$TMP/build.log" 2>&1 \
    || { sed -n '1,12p' "$TMP/build.log"; fail "oaep_sha1.ae did not build"; }
got=$(KEY="$TMP/k.der" MSG="$TMP/m.bin" CT="$TMP/c.bin" OUT="$TMP/a.bin" "$TMP/oaep_sha1" 2>&1)

fails=0
case "$got" in
    *"decrypt: openssl's ciphertext opens to the message"*) echo "    ok: openssl encrypts (OAEP-SHA1), Aether decrypts" ;;
    *) echo "    FAIL: Aether decrypt: [$got]"; fails=$((fails + 1)) ;;
esac
# shellcheck disable=SC2086
if [ -s "$TMP/a.bin" ] && openssl pkeyutl -decrypt -inkey "$TMP/k.pem" -in "$TMP/a.bin" -out "$TMP/a.out" $OAEP1 >"$TMP/dec.log" 2>&1 \
    && same_bytes "$TMP/a.out" "$TMP/m.bin"; then
    echo "    ok: Aether encrypts (OAEP-SHA1), openssl decrypts"
else
    echo "    FAIL: openssl decrypt of Aether's ciphertext: [$got] [$(head -3 "$TMP/dec.log" | tr '\n' '|')]"
    fails=$((fails + 1))
fi
[ "$fails" = 0 ] || fail "$fails case(s)"
echo "  [PASS] $NAME"
