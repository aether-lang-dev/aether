#!/bin/sh
# Install the duktape headers by copying them from the pinned
# hosted-language-headers checkout (clone-host-headers.sh), then install
# the runtime library end-user binaries dlopen.
# The other install-*-headers.sh scripts follow this pattern.

set -eu

HEADERS_DIR="${HEADERS_DIR:-/opt/hosted-language-headers}"

mkdir -p /opt/aether/include
cp -r "$HEADERS_DIR/js"/*.h /opt/aether/include/

test -f /opt/aether/include/duktape.h || {
    echo "install-js-headers: duktape.h missing" >&2
    exit 1
}

apt-get update
apt-get install -y --no-install-recommends libduktape207
rm -rf /var/lib/apt/lists/*

echo "install-js-headers: $(du -sh /opt/aether/include/duktape.h /opt/aether/include/duk_config.h 2>/dev/null | cut -f1 | xargs) at /opt/aether/include/"
