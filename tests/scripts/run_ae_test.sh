#!/bin/sh
# Build and run one .ae test, recording the outcome as marker files in $tmpdir
# for the sweep in the Makefile's `test-ae` target to tally afterwards.
#
#   run_ae_test.sh <test.ae> <tmpdir> <repo-root>
#
# Invoked once per test file, in parallel, by `xargs -P` from `test-ae`.
#
# Why this is a real file rather than being generated into $tmpdir by the
# recipe (which is what it used to be, ~35 `printf` lines):
#
#   `mingw32-make` passes a recipe to `sh -c` over a Windows command line that
#   is capped at 8 KB. The generated-script version pushed the single joined
#   recipe line to 11,679 bytes, so `sh` received exactly 8,191 of them — cut
#   mid-`printf`, leaving an unterminated quote:
#
#       /usr/bin/sh: -c: line 78: unexpected EOF while looking for matching `''
#       mingw32-make: *** [makefile:1235: test-ae] Error 2
#
#   That made `make test-ae` (and so `make test-all` / `make ci`) fail outright
#   on a Windows source build. CI never caught it because the Windows job runs
#   the MSYS2 `make`, a Cygwin-style binary that execs `sh` directly and has no
#   such cap — but README tells users to install `mingw-w64-x86_64-make`, which
#   *is* `mingw32-make`.
#
#   `sweep_resource_probe.sh` next door exists for the same family of reason:
#   non-trivial shell does not survive Makefile escaping intact.
#
# Behaviour is unchanged from the generated version on every platform.
set -u

f="$1"
tmpdir="$2"
root="$3"

# Portable per-test timeout: GNU coreutils `timeout` on Linux/MSYS2,
# `gtimeout` on macOS (coreutils via brew); empty when neither exists
# (macOS without coreutils) so the test still runs, just unbounded.
# The build gets a bound of its own (#2608): it ran unbounded, so a build that
# hung held its sweep slot, and the sweep, for as long as the job lasted.
if command -v timeout >/dev/null 2>&1; then
    TO="timeout ${AE_TEST_TIMEOUT:-120}"
    BTO="timeout ${AE_TEST_BUILD_TIMEOUT:-600}"
elif command -v gtimeout >/dev/null 2>&1; then
    TO="gtimeout ${AE_TEST_TIMEOUT:-120}"
    BTO="gtimeout ${AE_TEST_BUILD_TIMEOUT:-600}"
else
    TO=""
    BTO=""
fi

name=$(echo "$f" | sed "s|tests/||;s|/|_|g;s|\.ae$||")
dir=$(dirname "$f")
base=$(basename "$f")

# A test with a lib/ beside it is built from inside its own directory so the
# compiler resolves that lib/ as the module root.
if [ -d "$dir/lib" ]; then
    cmd="cd $dir && $root/build/ae build $base ${AE_BUILD_FLAGS:-} -o $root/build/test_$name"
else
    cmd="$root/build/ae build $f ${AE_BUILD_FLAGS:-} -o $root/build/test_$name"
fi

# Build and run times for the report at the end of the sweep (#2673).
. "$root/tests/scripts/sweep_clock.sh"
sweep_clock; t_start=$sweep_ms

$BTO sh -c "$cmd" 2>"$tmpdir/build_$name.err"
brc=$?
sweep_clock; t_built=$sweep_ms
t_ran=$t_built
if [ $brc -eq 0 ]; then
    $TO "$root/build/test_$name" >"$tmpdir/run_$name.out" 2>"$tmpdir/run_$name.err"
    rc=$?
    sweep_clock; t_ran=$sweep_ms
    if [ $rc -eq 0 ]; then
        echo "  [PASS] $name"
        touch "$tmpdir/PASS_$name"
    elif [ $rc -eq 124 ]; then
        echo "  [TIMEOUT] $name (exceeded ${AE_TEST_TIMEOUT:-120}s)"
        printf timeout > "$tmpdir/phase_$name.txt"
        touch "$tmpdir/FAIL_$name"
    else
        echo "  [FAIL] $name (runtime error, exit $rc)"
        printf runtime > "$tmpdir/phase_$name.txt"
        printf %s "$rc" > "$tmpdir/rc_$name.txt"
        touch "$tmpdir/FAIL_$name"
    fi
elif [ $brc -eq 124 ]; then
    echo "  [TIMEOUT] $name (build exceeded ${AE_TEST_BUILD_TIMEOUT:-600}s)"
    printf timeout > "$tmpdir/phase_$name.txt"
    touch "$tmpdir/FAIL_$name"
else
    echo "  [FAIL] $name (compile error)"
    printf compile > "$tmpdir/phase_$name.txt"
    touch "$tmpdir/FAIL_$name"
    head -5 "$tmpdir/build_$name.err" 2>/dev/null
fi

if [ -n "$t_start" ] && [ -n "$t_ran" ]; then
    printf '%s %s %s\n' $((t_built - t_start)) $((t_ran - t_built)) "$name" > "$tmpdir/time_$name.txt"
fi
