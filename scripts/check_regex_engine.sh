#!/bin/sh
# Fails when a cross-built libaether.a carries no regex engine.
#
# A cross build cannot use the host's libpcre2-8 (pkg-config would report the
# host's library), so std.regex there has to be the vendored engine
# (std/regex/pcre2/, compiled by aether_pcre2_vendored.c under
# AETHER_VENDOR_PCRE2). The FreeBSD cross build set PCRE2 := 0 instead, and
# 0.897.0's FreeBSD release shipped std.regex as its refusing stub: every
# regex.compile failed with "built without libpcre2-8", and nothing in the
# release job noticed, since the FreeBSD artifact is built, not tested.
#
# The vendored engine is in the archive when it exports pcre2_compile_8.
# Only for archives built with the vendored engine: a native build links the
# system libpcre2-8 instead and has no such symbol of its own.

set -e

ARCHIVE="$1"
if [ -z "$ARCHIVE" ] || [ ! -f "$ARCHIVE" ]; then
    echo "  [FAIL] regex engine check: no archive '$ARCHIVE'"
    exit 1
fi
NM="${NM:-nm}"
# Mach-O prefixes an underscore and ELF does not; accept either.
if "$NM" "$ARCHIVE" 2>/dev/null | grep -Eq ' T _?pcre2_compile_8$'; then
    echo "  [PASS] $ARCHIVE has the vendored regex engine"
else
    echo "  [FAIL] $ARCHIVE has no regex engine: std.regex there is the stub"
    echo "         (a cross build needs PCRE2 := vendored, as FREEBSD=1, WINDOWS=1"
    echo "          and ANDROID=1 set it in the Makefile)"
    exit 1
fi
