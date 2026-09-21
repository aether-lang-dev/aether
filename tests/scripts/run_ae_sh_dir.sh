#!/bin/sh
# Run every test_*.sh directly inside one integration directory, recording each
# outcome as marker files in $tmpdir for `test-ae` to tally afterwards.
#
#   run_ae_sh_dir.sh <dir> <tmpdir> <repo-root>
#
# Invoked once per directory by `xargs -P` from the Makefile's `test-ae`
# target. Only maxdepth 1, so each directory owns exactly its own tests.
#
# Lives in a real file rather than being generated into $tmpdir by the recipe
# for the reason documented at length in run_ae_test.sh beside it: the
# generated form pushed `test-ae`'s single joined recipe line past the 8 KB
# Windows command-line cap that `mingw32-make` passes recipes through, and the
# truncated tail broke the sweep on every Windows source build.
#
# Behaviour is unchanged from the generated version on every platform.
set -u

dir="$1"
tmpdir="$2"
root="$3"

# Portable per-test timeout, same shape as run_ae_test.sh. Shell tests get a
# longer default than .ae tests: they drive whole toolchain round-trips.
if command -v timeout >/dev/null 2>&1; then
    TO="timeout ${AE_SH_TEST_TIMEOUT:-420}"
elif command -v gtimeout >/dev/null 2>&1; then
    TO="gtimeout ${AE_SH_TEST_TIMEOUT:-420}"
else
    TO=""
fi

for sh_test in $(find "$dir" -maxdepth 1 -name "test_*.sh" 2>/dev/null | sort); do
    name=$(echo "$sh_test" | sed "s|tests/||;s|/|_|g;s|\.sh$||")
    sh "$root/tests/scripts/sweep_resource_probe.sh" "$name" 2>/dev/null
    $TO bash "$sh_test" >"$tmpdir/run_$name.out" 2>"$tmpdir/run_$name.err"
    sh_rc=$?
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
        echo "  [TIMEOUT] $name (shell test exceeded ${AE_SH_TEST_TIMEOUT:-420}s)"
        printf timeout > "$tmpdir/phase_$name.txt"
        touch "$tmpdir/FAIL_$name"
    elif [ $sh_rc -gt 128 ] && [ $sh_rc -lt 160 ]; then
        sig=$((sh_rc - 128))
        case $sig in
            9)  what="SIGKILL - killed outright; on a CI runner this is normally the OOM/resource killer" ;;
            11) what="SIGSEGV - segfault" ;;
            6)  what="SIGABRT - abort()/assert" ;;
            15) what="SIGTERM - asked to stop" ;;
            *)  what="signal $sig" ;;
        esac
        echo "  [SIGNAL] $name - $what (rc=$sh_rc)"
        printf signal > "$tmpdir/phase_$name.txt"
        touch "$tmpdir/FAIL_$name"
    else
        echo "  [FAIL] $name (shell test)"
        printf shell > "$tmpdir/phase_$name.txt"
        touch "$tmpdir/FAIL_$name"
    fi
done
