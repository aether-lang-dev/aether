#!/bin/sh
# See install-js-headers.sh for the pattern. Ruby has two
# include trees (portable + arch-specific); both are copied.

set -eu

HEADERS_DIR="${HEADERS_DIR:-/opt/hosted-language-headers}"

mkdir -p /opt/aether/include
cp -r "$HEADERS_DIR/ruby"      /opt/aether/include/ruby
cp -r "$HEADERS_DIR/ruby-arch" /opt/aether/include/ruby-arch

test -f /opt/aether/include/ruby/ruby.h || {
    echo "install-ruby-headers: ruby.h missing" >&2
    exit 1
}

apt-get update
apt-get install -y --no-install-recommends ruby
rm -rf /var/lib/apt/lists/*

echo "install-ruby-headers: $(du -sh /opt/aether/include/ruby /opt/aether/include/ruby-arch | cut -f1 | xargs) at /opt/aether/include/ruby{,-arch}"
