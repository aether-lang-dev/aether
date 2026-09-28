#!/bin/sh
# tests/run_macos_leaks.sh — macOS leaks(1) gate for the memory-
# ownership regression suite (#468).
#
# Why this exists: macOS has no working leak sanitizer. Apple's
# bundled clang ships ASan WITHOUT LeakSanitizer, and upstream LLVM's
# LSan hangs on Apple Silicon (sanitizers#1026, LLVM discourse 73148).
# Linux CI gets leak coverage from Valgrind (`make test-valgrind`) and
# ASan-with-LSan; macOS got nothing — which is exactly how the #471
# CI failures slipped past local macOS runs. Apple's `leaks(1)` tool
# is the one detector that works here: it inspects the heap at exit
# with no rebuild and no sanitizer flag.
#
# Scope: the FULL tests/regression suite. Every regression program is
# built with `ae build` (release -O2) and run under
# `MallocStackLogging=1 leaks --atExit`; a non-zero leak count fails the
# gate. Two audited side-lists qualify this:
#
#   tests/leaks_known.txt    — per-test MAX allowed leak count (third-
#                              party library process-globals + tracked
#                              known leaks). A test absent here must
#                              report 0; a test present may report up to
#                              its cap. Exceeding the cap fails.
#   tests/leaks_exclude.txt  — programs leaks(1) cannot run (they fork
#                              child processes; leaks --atExit hangs).
#                              Leak-checked on Linux CI instead.
#
# MallocStackLogging adds ~10x runtime, so the full sweep is slower than
# the old curated subset — but it is the only way a new leak in an
# un-curated program fails locally rather than slipping to CI. The
# programs are checked side by side, one per core (LEAKS_JOBS to choose),
# which is what keeps the sweep inside CI's time cap as the suite grows.
#
# Skips cleanly (exit 0) on non-Darwin hosts.

# NOTE: deliberately no `set -e`. Every build / leaks invocation has its
# exit code captured and handled explicitly below, and leaks(1) itself
# exits non-zero whenever it reports leaks — including the audited
# allowed counts in leaks_known.txt — so `set -e` would abort the whole
# gate on the first allowed leak.

if [ "$(uname -s)" != "Darwin" ]; then
    echo "[SKIP] test-macos-leaks: leaks(1) is macOS-only (Linux uses Valgrind/ASan)"
    exit 0
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
AE="$ROOT/build/ae"
REG="$ROOT/tests/regression"

if [ ! -x "$AE" ]; then
    echo "[SKIP] test-macos-leaks: build/ae not built (run: make ae)"
    exit 0
fi

# Full regression suite, discovered from disk (sorted for stable order).
TESTS="$(for f in "$REG"/test_*.ae; do basename "$f" .ae; done | sort)"

KNOWN_FILE="$ROOT/tests/leaks_known.txt"
EXCLUDE_FILE="$ROOT/tests/leaks_exclude.txt"

# Max allowed leaks for $1 (0 if not listed in leaks_known.txt).
allowed_leaks() {
    [ -f "$KNOWN_FILE" ] || { echo 0; return; }
    awk -v t="$1" '!/^#/ && NF>=2 && $1==t { print $2; found=1 } END { if (!found) print 0 }' "$KNOWN_FILE" | head -1
}

# True if $1 is listed in leaks_exclude.txt.
is_excluded() {
    [ -f "$EXCLUDE_FILE" ] || return 1
    grep -qE "^[[:space:]]*$1[[:space:]]*$" "$EXCLUDE_FILE"
}

# Per-step timeout with a PROCESS-GROUP kill so a stuck step can't hang
# the whole CI job until the 6-hour ceiling. This must kill the group,
# not just the direct child: `leaks --atExit -- prog` forks `prog`, and a
# plain `timeout`/`gtimeout` (or single-pid kill) only kills `leaks` —
# leaving `prog` orphaned and still holding the captured output open, so
# the caller hangs anyway. We fork the command into its OWN process group
# and, on timeout, `kill -KILL` the whole group (-pid), taking `leaks`
# AND `prog` down together. perl ships on every macOS runner. Exit code
# 124 == timed out.
run_bounded() {
    _secs="$1"; shift
    perl -e '
        use POSIX ();
        my $s = shift @ARGV;
        my $pid = fork();
        if (!defined $pid) { exit 127; }
        if ($pid == 0) { POSIX::setpgid(0, 0); exec(@ARGV); exit 127; }
        my $timed_out = 0;
        local $SIG{ALRM} = sub { $timed_out = 1; kill("KILL", -$pid); };
        alarm($s);
        waitpid($pid, 0);
        my $rc = $?;
        alarm(0);
        exit(124) if $timed_out;
        exit( ($rc & 127) ? (128 + ($rc & 127)) : ($rc >> 8) );
    ' "$_secs" "$@"
}

# Build timeout is generous (cold stdlib relink on a loaded runner);
# leaks(1) + MallocStackLogging runs ~10x slower than the bare program.
BUILD_TIMEOUT=240
LEAKS_TIMEOUT=120

# Build and leak-check one regression program. Prints its progress line at
# once and its verdict as one block when done, and leaves a marker in
# $tmpdir -- pass_<t>, fail_<t> or skip_<t> -- for the parent to count.
check_one() {
    t="$1"
    src="$REG/$t.ae"
    if [ ! -f "$src" ]; then
        echo "  [MISS] $t.ae not found — skipping"
        return
    fi
    if is_excluded "$t"; then
        echo "  [SKIP] $t: excluded (forks subprocesses — leak-checked on Linux CI)"
        : >"$tmpdir/skip_$t"
        return
    fi
    bin="$tmpdir/$t"
    report="$tmpdir/report_$t"
    # Newline-terminated progress line (NOT \r) so it flushes and is
    # visible in the non-TTY CI log — every test in flight is named, so a
    # slow/stuck step is obvious from the live output.
    echo "  [ .. ] $t: build + leak-check ..."
    run_bounded "$BUILD_TIMEOUT" "$AE" build "$src" -o "$bin" >"$tmpdir/build_$t.log" 2>&1
    brc=$?
    if [ "$brc" -eq 124 ]; then
        echo "  [FAIL] $t: build TIMED OUT (>${BUILD_TIMEOUT}s)"
        : >"$tmpdir/fail_$t"
        return
    elif [ "$brc" -ne 0 ]; then
        {
            echo "  [FAIL] $t: build failed"
            sed 's/^/      /' "$tmpdir/build_$t.log" | tail -10
        } >"$report"
        cat "$report"
        : >"$tmpdir/fail_$t"
        return
    fi
    # Capture via a FILE, not a `$(...)` pipe: with a pipe, a leaks(1) run
    # whose target program doesn't exit would orphan the program holding
    # the pipe's write end, and the command substitution would block even
    # after run_bounded kills the process group. A file has no such reader
    # dependency — the shell waits only for run_bounded.
    lk_out="$tmpdir/leaks_$t.out"
    MallocStackLogging=1 run_bounded "$LEAKS_TIMEOUT" leaks --atExit -- "$bin" >"$lk_out" 2>&1
    lrc=$?
    if [ "$lrc" -eq 124 ]; then
        # `leaks` runs the target in its own process group, so the
        # run_bounded group-kill above won't reach an orphaned target —
        # reap it by path so a stuck program can't accumulate across the
        # run. (Harmless if already gone.)
        pkill -9 -f "$bin" 2>/dev/null || true
        echo "  [FAIL] $t: leaks(1) TIMED OUT (>${LEAKS_TIMEOUT}s) — program did not exit"
        : >"$tmpdir/fail_$t"
        return
    fi
    # `leaks` prints "Process N: K leaks for B total leaked bytes."
    count="$(sed -nE 's/.*: ([0-9]+) leaks for [0-9]+ total.*/\1/p' "$lk_out" | head -1)"
    count="${count:-0}"
    allow="$(allowed_leaks "$t")"
    if [ "$count" -le "$allow" ]; then
        if [ "$allow" -gt 0 ]; then
            echo "  [PASS] $t: $count leaks (allowed ≤ $allow — see tests/leaks_known.txt)"
        else
            echo "  [PASS] $t: 0 leaks"
        fi
        : >"$tmpdir/pass_$t"
    else
        {
            echo "  [FAIL] $t: $count leaks (allowed ≤ $allow)"
            grep -E "ROOT LEAK|leaks for" "$lk_out" | head -12 | sed 's/^/      /'
        } >"$report"
        cat "$report"
        : >"$tmpdir/fail_$t"
    fi
    rm -f "$bin" "$lk_out" "$tmpdir/build_$t.log"
}

# A worker: `run_macos_leaks.sh --one TEST TMPDIR`, one per program, run
# by the fan-out below.
if [ "${1:-}" = "--one" ]; then
    tmpdir="$3"
    check_one "$2"
    exit 0
fi

echo "=================================================="
echo "macOS leaks(1) gate — full regression suite"
echo "=================================================="

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

# The programs are independent, so they are checked side by side, one per
# core: the suite is several hundred programs and leaks(1) with
# MallocStackLogging is ~10x the bare run, so one at a time outgrows the
# step's time cap as the suite grows. LEAKS_JOBS overrides the count.
jobs="${LEAKS_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 2)}"
echo "$(printf '%s\n' $TESTS | wc -l | tr -d ' ') programs, $jobs at a time"
printf '%s\n' $TESTS | xargs -P "$jobs" -I{} sh "$0" --one {} "$tmpdir"

covered=$(ls "$tmpdir" | grep -c '^pass_\|^fail_')
excluded=$(ls "$tmpdir" | grep -c '^skip_')
fails=$(ls "$tmpdir" | grep -c '^fail_')

echo "--------------------------------------------------"
echo "covered $covered programs, $excluded excluded, $fails over budget"
if [ "$fails" -ne 0 ]; then
    echo "FAIL: macOS leaks gate found leaks above the audited budget:"
    ls "$tmpdir" | sed -n 's/^fail_/  /p'
    exit 1
fi
echo "PASS: macOS leaks gate clean (within tests/leaks_known.txt budget)"
