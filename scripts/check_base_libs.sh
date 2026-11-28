#!/bin/sh
# Fails when a cross-built libaether.a was built without the libraries its
# target's base system ships: OpenSSL (libssl, libcrypto) and zlib.
#
# A cross build cannot ask the host's pkg-config about them, so the FreeBSD
# cross build set OPENSSL := 0 and ZLIB := 0, and every FreeBSD release up to
# 0.949.0 shipped std.zlib and the OpenSSL half of the stdlib as their stubs:
# zlib.available() was 0, gzip refused, and std.http's OpenSSL TLS path and
# std.cryptography's OpenSSL-only algorithms were missing. Nothing
# in the release job noticed, since the FreeBSD artifact is built, not
# tested. FreeBSD ships both in its base system, so the cross build now
# links the base sysroot's -lssl -lcrypto -lz.
#
# Each backend is compiled in when the archive calls into its library: an
# undefined reference to SSL_CTX_new (TLS), EVP_DigestInit_ex (the digests)
# and inflateInit2_ (gzip_inflate's entry point).

set -e

ARCHIVE="$1"
if [ -z "$ARCHIVE" ] || [ ! -f "$ARCHIVE" ]; then
    echo "  [FAIL] base library check: no archive '$ARCHIVE'"
    exit 1
fi
NM="${NM:-nm}"
syms="$("$NM" "$ARCHIVE" 2>/dev/null)"
missing=""
for s in SSL_CTX_new EVP_DigestInit_ex inflateInit2_; do
    # Mach-O prefixes an underscore and ELF does not; accept either.
    printf '%s\n' "$syms" | grep -Eq " U _?$s\$" || missing="$missing $s"
done
if [ -z "$missing" ]; then
    echo "  [PASS] $ARCHIVE links OpenSSL and zlib"
else
    echo "  [FAIL] $ARCHIVE does not call:$missing"
    echo "         OpenSSL or zlib there is the stub (a cross build whose sysroot"
    echo "         ships them needs OPENSSL := base and ZLIB := base, as FREEBSD=1"
    echo "         sets them in the Makefile)"
    exit 1
fi
