#!/bin/sh
# Does `-lz` link? Echo the flag if so, nothing otherwise (#1690).
#
# pkg-config is the first probe in the Makefile and the right one where
# it works. It does not work on macOS: the SDK ships libz and zlib.h but
# no zlib.pc, so the pkg-config query fails while -lz links and runs
# fine. Detection then reported "no zlib", every gzip response came back
# uncompressed, and http_middleware_d2 failed on that leg with no hint
# that a dependency was missing.
#
# Compiling and linking answers the question that actually matters —
# will this link — rather than looking for a metadata file that a
# platform may simply not ship. Lives in a script because the same logic
# inline in a $(shell ...) needs line continuations that GNU Make parses
# differently across platforms.
set -u
CC="${1:-cc}"
tmp="${TMPDIR:-/tmp}/aether_zlib_probe_$$"
trap 'rm -f "$tmp" "$tmp.c"' EXIT INT TERM
cat > "$tmp.c" <<'PROBE'
#include <zlib.h>
int main(void) { return zlibVersion() == 0; }
PROBE
if $CC -o "$tmp" "$tmp.c" -lz >/dev/null 2>&1; then
    echo "-lz"
fi
