#!/bin/sh
# AETHER_CFLAGS_EXTRA and AETHER_LDFLAGS_EXTRA, from the environment, are
# added to every compile and link `ae build` / `ae run` make, after
# aether.toml's [build] cflags / link_flags. The closure_*_no_uaf tests set
# them to build their probes under ASan, but nothing read them: the probes
# built uninstrumented, so the heap-use-after-free those tests exist to
# catch could never be reported.
#
# shim.c compiles only with the -D the environment gives and the one the
# manifest gives; a link flag naming a library that does not exist must
# fail the link (so it reached the link).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

fail=0
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
cd "$SCRIPT_DIR" || exit 1

if ! AETHER_CFLAGS_EXTRA="-DAE_EXTRA_MARKER=21" "$AE" build probe.ae -o "$tmpdir/probe" >"$tmpdir/build.log" 2>&1; then
    echo "  [FAIL] ae_cflags_extra_env: ae build did not apply AETHER_CFLAGS_EXTRA"
    sed 's/^/    /' "$tmpdir/build.log" | head -10
    fail=1
elif ! "$tmpdir/probe" | grep -q "^OK$"; then
    echo "  [FAIL] ae_cflags_extra_env: the probe built with AETHER_CFLAGS_EXTRA did not print OK"
    fail=1
fi

if ! AETHER_CFLAGS_EXTRA="-DAE_EXTRA_MARKER=21" "$AE" run probe.ae >"$tmpdir/run.log" 2>&1 ||
   ! grep -q "^OK$" "$tmpdir/run.log"; then
    echo "  [FAIL] ae_cflags_extra_env: ae run did not apply AETHER_CFLAGS_EXTRA"
    sed 's/^/    /' "$tmpdir/run.log" | head -10
    fail=1
fi

# Without it the shim's #error fires: the variable, not something else, made it build.
if "$AE" build probe.ae -o "$tmpdir/probe_plain" >"$tmpdir/plain.log" 2>&1; then
    echo "  [FAIL] ae_cflags_extra_env: built without AETHER_CFLAGS_EXTRA; the probe proves nothing"
    fail=1
fi

if AETHER_CFLAGS_EXTRA="-DAE_EXTRA_MARKER=21" AETHER_LDFLAGS_EXTRA="-lae_no_such_library_xyz" \
       "$AE" build probe.ae -o "$tmpdir/probe_ld" >"$tmpdir/ld.log" 2>&1; then
    echo "  [FAIL] ae_cflags_extra_env: AETHER_LDFLAGS_EXTRA did not reach the link"
    fail=1
fi

[ "$fail" -eq 0 ] && echo "  [PASS] ae_cflags_extra_env: AETHER_CFLAGS_EXTRA / AETHER_LDFLAGS_EXTRA reach the compile and the link, beside aether.toml's flags"
exit $fail
