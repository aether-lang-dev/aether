#!/bin/sh
# Run every test_*.sh directly inside one integration directory, recording each
# outcome as marker files in $tmpdir for `test-ae` to tally afterwards.
#
#   run_ae_sh_dir.sh <dir-or-script> <tmpdir> <repo-root>
#
# Invoked once per unit by `xargs -P` from the Makefile's `test-ae` target.
# A unit is normally a directory (only maxdepth 1, so each directory owns
# exactly its own tests, and its drivers run one after another: two drivers
# in one directory may share fixed ports or build outputs). A directory that
# holds a `DRIVERS_INDEPENDENT` marker file declares its drivers free of
# such sharing; the Makefile then hands each of its scripts over as a unit
# of its own, so they spread across the workers (#2132 §5: the nine
# wycheproof suites took 76% of the shell phase on one worker while three
# idled).
#
# Lives in a real file rather than being generated into $tmpdir by the recipe
# for the reason documented at length in run_ae_test.sh beside it: the
# generated form pushed `test-ae`'s single joined recipe line past the 8 KB
# Windows command-line cap that `mingw32-make` passes recipes through, and the
# truncated tail broke the sweep on every Windows source build.
#
# Behaviour is unchanged from the generated version on every platform.
set -u

# The tree is what the sweep tests, never an install: an inherited AETHER_HOME
# (install.sh sets it) points a test that runs build/aetherc without pinning
# it at the installed std, which fails where the two differ (#3342). A test
# that wants an install names one itself.
unset AETHER_HOME

unit="$1"
tmpdir="$2"
root="$3"
if [ -f "$unit" ]; then
    dir="$(dirname "$unit")"
    scripts="$unit"
else
    dir="$unit"
    scripts="$(find "$dir" -maxdepth 1 -name "test_*.sh" 2>/dev/null | sort)"
fi

# Per-test time limit, the same as run_ae_test.sh's (on Windows a job object
# around the test, never a signal: #2758, tests/lib/sweep_timeout.sh). Shell
# tests get a longer default than .ae tests: they drive whole toolchain
# round-trips.
. "$root/tests/lib/sweep_timeout.sh"
sweep_timeout_bound TO "$root" "${AE_SH_TEST_TIMEOUT:-420}"

# A test whose own work is long by nature (several full runtime builds for a
# cross target) says so with a line `# sweep-timeout: SECONDS` in its first
# 40 lines. That raises ITS limit above the default, never lowers it, so a
# hang in any other test is still cut off at the default. The limit is for the
# slowest runner, not the typical one: cross_ios takes 283 s on the macOS
# x86_64 runner on a good day and 393 s on a bad one, against a 420 s default.
test_timeout() {
    t=$(head -n 40 "$1" | sed -n 's/^# sweep-timeout: *\([0-9][0-9]*\).*/\1/p' | head -n 1)
    d="${AE_SH_TEST_TIMEOUT:-420}"
    if [ -n "$t" ] && [ "$t" -gt "$d" ]; then echo "$t"; else echo "$d"; fi
}

# Each test's wall time, for the report at the end of the sweep (#2673).
. "$root/tests/scripts/sweep_clock.sh"

# The last lines of a failed test's stdout and stderr, printed right under its
# [FAIL] / [TIMEOUT] / [SIGNAL] line. The summary's FAILURE DETAILS repeats
# them in full, but only if the sweep reaches the summary: a CI job that dies
# or is cancelled part-way through the shell phase used to leave nothing but
# the [FAIL] line to go on (#2391).
print_tail() {
    for stream in out err; do
        f="$tmpdir/run_$1.$stream"
        if [ -s "$f" ]; then
            echo "      ($stream, last ${AE_SH_FAIL_TAIL:-20} lines)"
            tail -n "${AE_SH_FAIL_TAIL:-20}" "$f" | sed 's/^/      | /'
        fi
    done
}

. "$root/tests/scripts/sweep_name.sh"
for sh_test in $scripts; do
    name=$(echo "$sh_test" | sed "$SWEEP_NAME_SED")
    sh "$root/tests/scripts/sweep_resource_probe.sh" "$name" 2>/dev/null
    sweep_clock; t_start=$sweep_ms
    # When the limit above ends this test, in seconds since the epoch: a test
    # that bounds steps of its own fits them under it, so its own report of a
    # stalled step is printed before the sweep kills it (cross_ios).
    this_timeout=$(test_timeout "$sh_test")
    sweep_timeout_bound TO "$root" "$this_timeout"
    AE_SH_TEST_DEADLINE=$(($(date +%s) + this_timeout))
    export AE_SH_TEST_DEADLINE
    $TO bash "$sh_test" >"$tmpdir/run_$name.out" 2>"$tmpdir/run_$name.err"
    sh_rc=$?
    sweep_clock
    if [ -n "$t_start" ] && [ -n "$sweep_ms" ]; then
        printf '%s %s\n' $((sweep_ms - t_start)) "$name" > "$tmpdir/shtime_$name.txt"
    fi
    if [ $sh_rc -eq 0 ]; then
        # A test that ran green but printed [SKIP-WIN] opted itself out on this
        # platform, and one whose output is a [SKIP] line and no PASS at all
        # (a host toolchain it needs is not installed) never ran its body:
        # both are recorded as skips, so the summary line says how many tests
        # did not run instead of folding them into the pass count (#2132). A
        # driver that skips one CASE and passes the rest prints a PASS line
        # and stays a pass.
        if grep -q "\[SKIP-WIN\]" "$tmpdir/run_$name.out" 2>/dev/null; then
            reason=$(grep "\[SKIP-WIN\]" "$tmpdir/run_$name.out" | head -1 | sed "s/^[[:space:]]*\[SKIP-WIN\][[:space:]]*//")
            echo "  [SKIP] $name — $reason"
            touch "$tmpdir/SKIP_$name"
        elif grep -q "^[[:space:]]*\[SKIP\]" "$tmpdir/run_$name.out" 2>/dev/null &&
             ! grep -q "PASS" "$tmpdir/run_$name.out" 2>/dev/null; then
            reason=$(grep "^[[:space:]]*\[SKIP\]" "$tmpdir/run_$name.out" | head -1 | sed "s/^[[:space:]]*\[SKIP\][[:space:]]*//")
            echo "  [SKIP] $name — $reason"
            touch "$tmpdir/SKIP_$name"
        else
            echo "  [PASS] $name"
            touch "$tmpdir/PASS_$name"
        fi
    elif [ $sh_rc -eq 124 ]; then
        echo "  [TIMEOUT] $name (shell test exceeded ${this_timeout}s)"
        print_tail "$name"
        printf timeout > "$tmpdir/phase_$name.txt"
        touch "$tmpdir/FAIL_$name"
    elif [ $sh_rc -gt 128 ] && [ $sh_rc -lt 160 ]; then
        sig=$((sh_rc - 128))
        case $sig in
            9)  what="SIGKILL - killed outright: the OOM/resource killer, or on Windows a signal sent to a native program's MSYS2 stand-in (#2479, tests/lib/server_jobs.sh)" ;;
            11) what="SIGSEGV - segfault" ;;
            6)  what="SIGABRT - abort()/assert" ;;
            15) what="SIGTERM - asked to stop" ;;
            *)  what="signal $sig" ;;
        esac
        echo "  [SIGNAL] $name - $what (rc=$sh_rc)"
        print_tail "$name"
        printf signal > "$tmpdir/phase_$name.txt"
        touch "$tmpdir/FAIL_$name"
    else
        echo "  [FAIL] $name (shell test)"
        print_tail "$name"
        printf shell > "$tmpdir/phase_$name.txt"
        touch "$tmpdir/FAIL_$name"
    fi
done
