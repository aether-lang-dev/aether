#!/bin/sh
# contrib/host/factor end-to-end smoke test.
#
# Skips cleanly unless BOTH env vars point at the embed-api fork
# (aether-lang-dev/factor-language):
#   AETHER_FACTOR_SONAME  - path to its built libfactor (.so)
#   AETHER_FACTOR_IMAGE   - path to its bootstrapped factor.image
# CI machines won't have these, so the test no-ops there (host bridges
# must never break CI on machines without the runtime). When set, it
# builds the factor host .a (the default contrib build skips factor since
# it needs the fork), then runs uses_factor.ae, which exercises
# eval-with-result, run, and the k-v map.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ -z "$AETHER_FACTOR_SONAME" ] || [ -z "$AETHER_FACTOR_IMAGE" ]; then
    echo "  [SKIP] AETHER_FACTOR_SONAME / AETHER_FACTOR_IMAGE unset"
    echo "         (needs the aether-lang-dev/factor-language fork: build its"
    echo "          libfactor + bootstrap factor.image, then point these env vars)"
    exit 0
fi
if [ ! -f "$AETHER_FACTOR_SONAME" ]; then
    echo "  [SKIP] AETHER_FACTOR_SONAME=$AETHER_FACTOR_SONAME not found"
    exit 0
fi
if [ ! -f "$AETHER_FACTOR_IMAGE" ]; then
    echo "  [SKIP] AETHER_FACTOR_IMAGE=$AETHER_FACTOR_IMAGE not found"
    exit 0
fi
if [ ! -x "$AE" ]; then
    echo "  [SKIP] host_factor: $AE not built"
    exit 0
fi

# The factor host isn't in the default contrib build (it needs the fork),
# so build its .a here, into the dir ae build's host-bridge scanner reads.
OUT="$ROOT/build/contrib"
mkdir -p "$OUT"
if ! cc -c -O2 -DAETHER_HAS_FACTOR -DAETHER_HAS_SANDBOX -I"$ROOT" \
        "$ROOT/contrib/host/factor/aether_host_factor.c" \
        -o "$OUT/host_factor.o" 2>/tmp/host_factor_cc.err; then
    echo "  [FAIL] host_factor: bridge compile failed"
    head -10 /tmp/host_factor_cc.err
    exit 1
fi
ar rcs "$OUT/libaether_host_factor.a" "$OUT/host_factor.o"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

ACTUAL="$TMPDIR/actual.txt"
if ! AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/uses_factor.ae" \
        >"$ACTUAL" 2>"$TMPDIR/err.log"; then
    echo "  [FAIL] host_factor: ae run exited non-zero"
    head -20 "$TMPDIR/err.log"
    head -10 "$ACTUAL"
    exit 1
fi

if grep -Fxq "PASS: factor host fib k-v map + eval round-trip" "$ACTUAL"; then
    echo "  [PASS] contrib.host.factor: fib k-v map + eval-with-result"
else
    echo "  [FAIL] host_factor: driver did not reach PASS"
    echo "--- actual ---"; cat "$ACTUAL"
    exit 1
fi
