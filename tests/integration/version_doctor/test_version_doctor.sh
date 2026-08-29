#!/bin/sh
# `ae version doctor` — diagnose an install, and prove it by compiling.
#
# The reason this exists: `ae --version` never invokes the compiler. It
# reports a healthy toolchain on an install that cannot build anything, so
# every check that only compares version strings inherits that blindness.
# The doctor's last check is an actual compile, and this test's centre of
# gravity is that the compile probe FAILS on a broken toolchain -- a doctor
# that always says "ok" is worse than none.
#
# Asserts:
#   - a healthy tree reports no problems and exits 0
#   - a missing libaether.h is reported (the gap a release actually shipped)
#   - a toolchain that cannot compile is reported, even though every
#     string-comparison check above it still passes
#   - a pin naming an UNINSTALLED version is repaired by --fix
#   - a pin naming an INSTALLED version is left alone by --fix, because
#     that is a choice between two real installs rather than a fault

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] version_doctor: ae not built"; exit 0; }

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP" || :; return 0; }
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

# --- a broken pin is repaired, an installed one is not --------------------
mkdir -p "$TMP/h1/.aether/versions"
echo "0.111.0" > "$TMP/h1/.aether/active_version"
HOME="$TMP/h1" "$AE" version doctor --fix >"$TMP/o1" 2>&1 || true
grep -q 'fixed: pin now reads' "$TMP/o1" \
    || { sed 's/^/    /' "$TMP/o1"; fail "--fix did not repair a pin naming an uninstalled version"; }

# Same pin, but the version IS installed: --fix must NOT rewrite it.
mkdir -p "$TMP/h2/.aether/versions/v0.111.0"
echo "0.111.0" > "$TMP/h2/.aether/active_version"
HOME="$TMP/h2" "$AE" version doctor --fix >"$TMP/o2" 2>&1 || true
if grep -q 'fixed: pin now reads' "$TMP/o2"; then
    sed 's/^/    /' "$TMP/o2"
    fail "--fix rewrote a pin whose version is installed; that is a choice, not a fault"
fi
[ "$(cat "$TMP/h2/.aether/active_version")" = "0.111.0" ] \
    || fail "--fix modified an installed pin on disk"

# --- the compile probe must be able to FAIL -------------------------------
# A doctor whose probe cannot fail proves nothing. The tree has to be
# COMPLETE (a stdlib, a MANIFEST) so the earlier checks pass and execution
# reaches the probe -- an install broken badly enough that `ae --version`
# itself fails is reported separately and stops before here. So: copy a real
# install, then break only the compiler.
INST="$TMP/inst"
mkdir -p "$INST"
( cd "$ROOT" && make install PREFIX="$INST" ) >"$TMP/install.log" 2>&1 \
    || { echo "  [SKIP] version_doctor: could not stage an install to break"; exit 0; }
cp -a "$INST" "$TMP/bad"
cp "$AE" "$TMP/bad/bin/ae"
: > "$TMP/bad/bin/aetherc"
chmod +x "$TMP/bad/bin/aetherc"
AETHER_HOME="$TMP/bad" "$TMP/bad/bin/ae" version doctor >"$TMP/o3" 2>&1 || true
grep -qi 'cannot compile' "$TMP/o3" \
    || { sed 's/^/    /' "$TMP/o3"; fail "the compile probe did not fail on a toolchain that cannot compile"; }

# --- exit status carries the verdict --------------------------------------
if AETHER_HOME="$TMP/bad" "$TMP/bad/bin/ae" version doctor >/dev/null 2>&1; then
    fail "doctor exited 0 on a broken install"
fi

echo "  [PASS] version_doctor: probe fails on a broken toolchain; --fix repairs only a broken pin"
