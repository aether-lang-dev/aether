#!/bin/sh
# std.sed against FreeBSD's own sed tests, and against the host's BSD sed.
#
# aesed.ae is FreeBSD's sed command line (main.c's main()) on std.sed. It is
# put first on PATH as `sed`, and FreeBSD's test scripts run unchanged
# (freebsd/, copied from usr.bin/sed/tests and usr.bin/tests/regress.m4 at
# the commit std/sed/PROVENANCE.md names):
#   - multi_test.sh: 130 tests against regress.multitest.out/;
#   - legacy_test.sh: regress.sh through regress.m4, 26 tests.
# sed2_test.sh needs ATF, which neither host has; its cases run through the
# library in std/sed/test_sed.ae. FreeBSD's inplace_race_test.sh (sed -i
# racing a reader of the file) has no library counterpart and is left out.
# The same scripts then run with the host's own sed, and every test the
# host's sed passes, aesed must pass too. On FreeBSD that is all of them;
# macOS's sed is FreeBSD's with a few changes, and its /bin/sh's echo and
# case-insensitive file system fail a few tests whichever sed runs them.
#
# Runs on FreeBSD and macOS (a BSD sed, m4, jot, diff, /usr/share/dict/words);
# elsewhere it skips, and std/sed/test_sed.ae runs the same cases through
# the library.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s)" in
    Darwin|FreeBSD) ;;
    *)
        echo "  [PASS] sed_parity: SKIP (FreeBSD's sed tests need a BSD host)"
        exit 0
        ;;
esac
for tool in m4 jot diff; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "  [PASS] sed_parity: SKIP (no $tool)"
        exit 0
    fi
done

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

fail() {
    echo "  [FAIL] sed_parity: $1"
    shift
    for f in "$@"; do
        [ -f "$f" ] && sed 's/^/    /' "$f" | tail -40
    done
    exit 1
}

mkdir -p "$TMPDIR/bin"
( cd "$TMPDIR" && "$ROOT/build/ae" build "$SCRIPT_DIR/aesed.ae" -o "$TMPDIR/bin/sed" ) \
    >"$TMPDIR/build.log" 2>&1 || fail "building aesed.ae" "$TMPDIR/build.log"

# Run one suite in a fresh directory; $1 names the run, $2 the PATH prefix
# ("" for the host's sed), $3 the script. Leaves the TAP output in
# $TMPDIR/$1.tap and the failing test names in $TMPDIR/$1.fail.
suite() {
    mkdir -p "$TMPDIR/$1"
    # A watchdog kills the suite after SUITE_TIMEOUT seconds (default 300), so
    # a script that loops cannot hang the sweep. (macOS has no timeout(1).)
    ( cd "$TMPDIR/$1" && PATH="$2$PATH" exec sh "$3" ) >"$TMPDIR/$1.tap" 2>&1 &
    suite_pid=$!
    ( sleep "${SUITE_TIMEOUT:-300}"; echo "  sed_parity: suite $1 timed out" >&2; pkill -KILL -P "$suite_pid" 2>/dev/null; kill -KILL "$suite_pid" 2>/dev/null ) &
    watchdog_pid=$!
    wait "$suite_pid"
    pkill -P "$watchdog_pid" 2>/dev/null; kill "$watchdog_pid" 2>/dev/null
    wait "$watchdog_pid" 2>/dev/null
    grep '^not ok' "$TMPDIR/$1.tap" | sed -e 's/^not ok [0-9]* *-* *//' -e 's/ #.*//' | sort >"$TMPDIR/$1.fail"
}

suite multi_ae "$TMPDIR/bin:" "$SCRIPT_DIR/freebsd/sed/multi_test.sh"
suite multi_sys "" "$SCRIPT_DIR/freebsd/sed/multi_test.sh"
suite legacy_ae "$TMPDIR/bin:" "$SCRIPT_DIR/freebsd/sed/legacy_test.sh"
suite legacy_sys "" "$SCRIPT_DIR/freebsd/sed/legacy_test.sh"

n_multi="$(grep -c '^ok' "$TMPDIR/multi_ae.tap")"
n_legacy="$(grep -c '^ok' "$TMPDIR/legacy_ae.tap")"
[ "$(grep -c '^ok\|^not ok' "$TMPDIR/multi_ae.tap")" -eq 130 ] \
    || fail "multi_test.sh did not run its 130 tests under aesed" "$TMPDIR/multi_ae.tap"
[ "$(grep -c '^ok\|^not ok' "$TMPDIR/legacy_ae.tap")" -eq 26 ] \
    || fail "legacy_test.sh did not run its 26 tests under aesed" "$TMPDIR/legacy_ae.tap"

# Tests aesed fails that the host's sed passes.
worse=""
for s in multi legacy; do
    for t in $(cat "$TMPDIR/${s}_ae.fail"); do
        if ! grep -qx "$t" "$TMPDIR/${s}_sys.fail"; then
            worse="$worse $s:$t"
        fi
    done
done
if [ -n "$worse" ]; then
    fail "aesed fails what $(uname -s)'s sed passes:$worse" "$TMPDIR/multi_ae.tap" "$TMPDIR/legacy_ae.tap"
fi
host_fails="$(cat "$TMPDIR/multi_sys.fail" "$TMPDIR/legacy_sys.fail" | tr '\n' ' ')"
echo "  [PASS] sed_parity: multi_test $n_multi/130, regress.sh $n_legacy/26 under std.sed; the host's sed fails: ${host_fails:-none}"
