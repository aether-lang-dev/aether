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
# The pin only governs an INSTALLED toolchain: a source tree resolves its
# compiler next to its own binary and never reads the pin, so these cases
# need an `ae` that is not in dev mode. Copy the binary somewhere with no
# runtime/ beside it AND run from a directory that is not a source tree --
# detection falls back to the CWD, so running from $ROOT is dev mode however
# the binary was invoked.
mkdir -p "$TMP/notdev/bin"
cp "$AE" "$TMP/notdev/bin/ae"
NOTDEV="$TMP/notdev/bin/ae"
mkdir -p "$TMP/elsewhere"

mkdir -p "$TMP/h1/.aether/versions"
echo "0.111.0" > "$TMP/h1/.aether/active_version"
( cd "$TMP/elsewhere" && HOME="$TMP/h1" "$NOTDEV" version doctor --fix ) >"$TMP/o1" 2>&1 || true
# The pin check only runs if the doctor got far enough to reach it. Where
# there is no usable install at all -- a CI runner that never ran `make
# install`, say -- it reports that and stops, which is the correct
# behaviour and leaves no pin verdict to assert. Skip rather than fail, so
# this exercises the pin logic where the pin logic actually runs.
if grep -q 'too broken to check further' "$TMP/o1"; then
    echo "  [SKIP] version_doctor: no usable install here to exercise the pin checks"
    exit 0
fi
grep -q 'fixed: pin now reads' "$TMP/o1" \
    || { sed 's/^/    /' "$TMP/o1"; fail "--fix did not repair a pin naming an uninstalled version"; }

# Same pin, but the version IS installed: --fix must NOT rewrite it.
mkdir -p "$TMP/h2/.aether/versions/v0.111.0"
echo "0.111.0" > "$TMP/h2/.aether/active_version"
( cd "$TMP/elsewhere" && HOME="$TMP/h2" "$NOTDEV" version doctor --fix ) >"$TMP/o2" 2>&1 || true
if grep -q 'fixed: pin now reads' "$TMP/o2"; then
    sed 's/^/    /' "$TMP/o2"
    fail "--fix rewrote a pin whose version is installed; that is a choice, not a fault"
fi
[ "$(cat "$TMP/h2/.aether/active_version")" = "0.111.0" ] \
    || fail "--fix modified an installed pin on disk"

# ...and it must SAY it repaired nothing, rather than exiting 1 in silence.
# Reported from real use: --fix on a set of findings that are all decisions
# rather than faults printed "3 problem(s) found." and exited 1, leaving the
# user unable to tell whether it had tried and failed or declined by design.
# Exit 1 is right (the problems are real); the silence was not.
grep -qi 'repaired nothing' "$TMP/o2" \
    || { sed 's/^/    /' "$TMP/o2"; fail "--fix repaired nothing but did not say so"; }

# The offer must also be honest in the other direction: without --fix, only
# advertise a repair when there is actually one to make.
( cd "$TMP/elsewhere" && HOME="$TMP/h2" "$NOTDEV" version doctor ) >"$TMP/o2b" 2>&1 || true
if grep -q 'Re-run with --fix' "$TMP/o2b"; then
    sed 's/^/    /' "$TMP/o2b"
    fail "doctor offered --fix when nothing it found was auto-repairable"
fi

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

# --- a dev tree is not an installed toolchain -----------------------------
# The pin governs INSTALLED toolchains. A source tree resolves its compiler
# next to its own binary and never consults it, and its version is a working
# build with no matching release -- so warning about a pin disagreement here
# would tell a developer to run `ae version use <version>` for a version that
# cannot be installed. The doctor must say the pin does not apply instead.
DEVOUT="$TMP/dev.out"
( cd "$ROOT" && "$AE" version doctor ) >"$DEVOUT" 2>&1 || true
grep -q 'not applicable: this is a source tree' "$DEVOUT" \
    || { sed 's/^/    /' "$DEVOUT"; fail "doctor applied the version pin to a source tree"; }

# And it must never propose a version that is not installed, in either mode.
while IFS= read -r line; do
    case "$line" in
        *"ae version use "*)
            v=$(printf '%s\n' "$line" | sed 's/.*ae version use \([0-9.]*\).*/\1/')
            [ -n "$v" ] || continue
            [ -d "$HOME/.aether/versions/v$v" ] \
                || fail "doctor suggested 'ae version use $v', which is not installed"
            ;;
    esac
done < "$DEVOUT"

echo "  [PASS] version_doctor: probe fails on a broken toolchain; --fix repairs only a broken pin; dev trees exempt from the pin"
