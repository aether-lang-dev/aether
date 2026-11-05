#!/bin/sh
# See install-js-headers.sh for the pattern.

set -eu

HEADERS_DIR="${HEADERS_DIR:-/opt/hosted-language-headers}"

mkdir -p /opt/aether/include
cp -r "$HEADERS_DIR/perl" /opt/aether/include/perl

test -f /opt/aether/include/perl/EXTERN.h || {
    echo "install-perl-headers: EXTERN.h missing" >&2
    exit 1
}

apt-get update
apt-get install -y --no-install-recommends perl-base
rm -rf /var/lib/apt/lists/*

echo "install-perl-headers: $(du -sh /opt/aether/include/perl | cut -f1) at /opt/aether/include/perl"
