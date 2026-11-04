#!/bin/sh
# Constant-time check of std.cryptography's secret-key operations, in the
# style of ctgrind and BoringSSL's constant-time validation.
#
# ct_probe.ae runs each operation (signing, key agreement, private-key
# decryption, the record ciphers and MACs) on a known-answer vector with its
# secret inputs marked undefined for Valgrind's memcheck (ct_shim.c). memcheck
# then reports every conditional jump or move, and every memory address,
# computed from a secret: a branch or a table index that makes the time or
# the cache footprint depend on the key. Values the code is entitled to act
# on go through std.cryptography.ct.declassify, which the probe hooks.
#
# The verdict: no memcheck error whose uninitialised value was created by the
# shim's client request (a secret), and every known answer right. Other
# memcheck errors are not this test's to judge (the memory-check workflow
# owns them); they are listed, not counted.
#
# The probe is built with the default `ae build`, the -O2 code a program
# ships, since that is the code whose branches matter.
#
# Without valgrind (Windows, macOS, a Linux box without it) the probe still
# runs as a known-answer test and the test reports a skip. CT_REQUIRE_VALGRIND=1
# (the CI job that installs valgrind) turns a missing valgrind into a failure.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"
[ -x "$AE" ] || { echo "  [FAIL] crypto_ct_valgrind: build/ae missing (run make)"; exit 1; }

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cd "$SCRIPT_DIR" || exit 1
if ! "$AE" build ct_probe.ae -o "$tmpdir/ct_probe" >"$tmpdir/build.log" 2>&1; then
    echo "  [FAIL] crypto_ct_valgrind: probe build failed"
    sed 's/^/    /' "$tmpdir/build.log" | tail -30
    exit 1
fi
probe="$tmpdir/ct_probe"
[ -x "$probe" ] || probe="$probe.exe"

have_vg=0
# Valgrind cannot run an AddressSanitizer build (both replace the
# allocator), so under ASan the probe runs as the known-answer test (#3539).
if [ "$(uname -s)" = "Linux" ] && command -v valgrind >/dev/null 2>&1 &&
   [ "${AETHER_SANITIZER:-}" != asan ]; then
    have_vg=1
fi

if [ "$have_vg" -eq 0 ]; then
    if ! "$probe" >"$tmpdir/run.log" 2>&1 || ! grep -q "^ALL PASS" "$tmpdir/run.log"; then
        echo "  [FAIL] crypto_ct_valgrind: known-answer run failed"
        sed 's/^/    /' "$tmpdir/run.log" | tail -30
        exit 1
    fi
    if [ "${CT_REQUIRE_VALGRIND:-0}" = "1" ]; then
        echo "  [FAIL] crypto_ct_valgrind: valgrind is required (CT_REQUIRE_VALGRIND=1) and not installed"
        exit 1
    fi
    echo "  [SKIP] crypto_ct_valgrind: no valgrind on this host (the known-answer run passed)"
    exit 0
fi

supp=""
[ -f "$ROOT/.valgrind-suppressions" ] && supp="--suppressions=$ROOT/.valgrind-suppressions"
# shellcheck disable=SC2086
valgrind --tool=memcheck --track-origins=yes --error-exitcode=0 \
    --num-callers=24 $supp --log-file="$tmpdir/vg.log" \
    "$probe" >"$tmpdir/run.log" 2>&1
rc=$?

# One paragraph per memcheck error once the "==pid== " prefix is gone.
sed 's/^==[0-9]*== \{0,1\}//' "$tmpdir/vg.log" > "$tmpdir/vg.txt"
awk 'BEGIN { RS = "" } /created by a client request/ { print; print "" }' \
    "$tmpdir/vg.txt" > "$tmpdir/secret_errors.txt"
nsecret=$(grep -c -E '^(Conditional jump|Use of uninitialised|Syscall param)' "$tmpdir/secret_errors.txt")
nother=$(grep -E '^ERROR SUMMARY: ' "$tmpdir/vg.txt" | tail -1 | awk '{ print $3 }')

fail=0
if [ "$rc" -ne 0 ] || ! grep -q "^ALL PASS" "$tmpdir/run.log"; then
    echo "  [FAIL] crypto_ct_valgrind: the probe failed under valgrind (exit $rc)"
    sed 's/^/    /' "$tmpdir/run.log" | tail -30
    fail=1
fi
if [ "$nsecret" -gt 0 ]; then
    echo "  [FAIL] crypto_ct_valgrind: $nsecret branch(es) or memory index(es) depend on a secret:"
    sed 's/^/    /' "$tmpdir/secret_errors.txt" | head -400
    fail=1
fi
[ "$fail" -eq 0 ] || exit 1
grep '^ok ' "$tmpdir/run.log" | sed 's/^ok */  [PASS] crypto_ct_valgrind: constant time: /'
echo "  [PASS] crypto_ct_valgrind: no secret-dependent branch or index (memcheck errors in total: ${nother:-0})"
exit 0
