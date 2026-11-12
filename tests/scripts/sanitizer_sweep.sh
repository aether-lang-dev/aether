#!/bin/sh
# `make test-ubsan` (#2488), `make test-asan-ae` (#3539) and `make test-tsan`
# (#3542): the C unit tests and the `make test-ae` sweep, with the toolchain,
# libaether and every program they build compiled under a sanitizer. Run from
# the repository root.
#
#   SAN=ubsan MAKE=make NPROC=4 BASE_CC=gcc sh tests/scripts/sanitizer_sweep.sh
#   SAN=asan  MAKE=make NPROC=4 BASE_CC=gcc sh tests/scripts/sanitizer_sweep.sh
#   SAN=tsan  MAKE=make NPROC=4 sh tests/scripts/sanitizer_sweep.sh
#
# SAN=ubsan is UndefinedBehaviorSanitizer; SAN=asan is AddressSanitizer with
# LeakSanitizer, so a heap overflow, a use after free or a double free stops
# the program, and a block it never freed is reported when it exits and
# fails the run (SAN_LEAKS=report lists it without failing). SAN=tsan is
# ThreadSanitizer under clang (TSAN_CC names another clang): a data race, a
# lock-order inversion, a thread never joined or a call that is not
# async-signal-safe in a signal handler stops the program. It runs the part
# of the corpus that puts two threads on the same memory, the actor,
# scheduler and std.net tests (tests/tsan_sweep_select.txt).
#
# Every compile and link goes through the sanitizing compiler: build/<san>/bin
# holds a `gcc` and a `cc` that run the real ones with the sanitizer's flags,
# put first on PATH and named by absolute path in CC (under TSan, `gcc`, `cc`
# and `clang` all run the one clang: an object instrumented by gcc's libtsan
# and one by clang's are not meant to share a program). That reaches what the
# Makefile compiles, what `ae build` compiles (it reads CC), the shell tests
# that run "$CC" and the ones that run gcc or cc by name, which a CFLAGS
# setting would not: an instrumented libaether.a does not even link into a
# program built without the sanitizer's runtime. CC names the wrapper by its
# path, not as "gcc", so the build-flags digest and ae's cache key both see
# another compiler, and an instrumented object or program is never mixed with
# a plain one.
#
# A report stops the program, so the test that ran it fails, and is written
# to build/<san>/reports/report.<program>.<pid> (the sanitizer's log_path),
# where a test that expects its program to fail, or compares its stderr,
# cannot hide it. The run fails on any report, and lists them grouped by
# source location (also into $GITHUB_STEP_SUMMARY in CI).
#
# Before the tests, two checks that the configuration took: build/libaether.a
# must call the sanitizer, and the canary under tests/<san>/, built by ae and
# run, must be stopped with a report (for ASan, a heap overflow and a leak,
# each in its own run; for TSan, a data race). Without them a run whose flags
# were lost would read as a clean corpus.
#
# Needs a C compiler with the sanitizer's runtime: gcc's libubsan / libasan or
# clang's compiler-rt (Linux, macOS). MinGW has none; there it stops and says
# so. AE_SWEEP_SHARD=k/n splits the .ae corpus as it does for `make test-ae`;
# the C unit tests run in shard 1 only.
set -u

# Flags that go after the command's own arguments, where a later one wins:
# only TSan's -O1, below.
tail_flags=""
san=${SAN:-ubsan}
case "$san" in
    ubsan)
        # The -fsanitize groups (the Makefile's UBSAN_CHECKS): gcc's
        # `undefined` does not include float-cast-overflow, a float converted
        # to an integer type it does not fit, which generated code saturates
        # since #3524.
        checks=${UBSAN_CHECKS:-undefined,float-cast-overflow}
        flags="-fsanitize=$checks -fno-sanitize-recover=$checks -fno-omit-frame-pointer"
        name="UBSan"
        hook="__ubsan_handle_"
        ;;
    asan)
        # -g: a report names file and line; without it each frame is an
        # address in the program, which groups nothing.
        flags="-fsanitize=address -fno-omit-frame-pointer -g"
        name="ASan + LSan"
        hook="__asan_report_"
        ;;
    tsan)
        # -g names file and line in both stacks of a race. -O1 is what TSan's
        # documentation asks for: the stacks keep their frames, and the run is
        # several times faster than at -O0. It goes last, so it is the level
        # every compile gets, the Makefile's -O2 and `ae run`'s -O0 included.
        flags="-fsanitize=thread -fno-omit-frame-pointer -g"
        tail_flags="-O1"
        name="TSan"
        hook="__tsan_"
        ;;
    *)
        echo "sanitizer_sweep.sh: SAN must be ubsan, asan or tsan, got '$san'" >&2
        exit 2
        ;;
esac

root=$(pwd)
dir="$root/build/$san"
bin="$dir/bin"
reports="$dir/reports"
make_cmd=${MAKE:-make}
nproc=${NPROC:-2}
base_cc=${BASE_CC:-gcc}

rm -rf "$bin" "$reports" "$dir/canary"
mkdir -p "$bin" "$reports" "$dir/canary" || exit 1

# PATH without the wrapper directory, so a wrapper never resolves to itself.
plain_path=$(printf '%s' "$PATH" | tr ':' '\n' | grep -v -x -F "$bin" | paste -s -d: -)
# wrap NAME REAL: build/<san>/bin/NAME runs REAL with the sanitizer's flags.
wrap() {
    printf '#!/bin/sh\n# %s under %s, written by tests/scripts/sanitizer_sweep.sh.\nexec "%s" %s "$@"%s\n' \
        "$1" "$name" "$2" "$flags" "${tail_flags:+ $tail_flags}" > "$bin/$1"
    chmod +x "$bin/$1"
}
if [ "$san" = tsan ]; then
    tsan_cc=${TSAN_CC:-clang}
    real=$(PATH=$plain_path command -v "$tsan_cc" 2>/dev/null)
    case "$real" in
        /*) ;;
        *) echo "test-tsan: no $tsan_cc on PATH (TSAN_CC names the clang to use)" >&2; exit 1 ;;
    esac
    for cc_name in gcc cc clang; do wrap "$cc_name" "$real"; done
    base_cc=clang
else
    for cc_name in gcc cc; do
        real=$(PATH=$plain_path command -v "$cc_name" 2>/dev/null) || continue
        case "$real" in /*) ;; *) continue ;; esac
        wrap "$cc_name" "$real"
    done
    case "$base_cc" in gcc|cc) ;; *) base_cc=cc ;; esac
fi
if [ ! -x "$bin/$base_cc" ]; then
    echo "test-$san: no $base_cc on PATH to wrap" >&2
    exit 1
fi
CC="$bin/$base_cc"
PATH="$bin:$PATH"
# Which sanitizer the programs run under, for the few tests that check
# something the sanitizer replaces (FORTIFY's abort, an address-space
# limit, Valgrind): they say so and take another path or skip.
AETHER_SANITIZER="$san"
export CC PATH AETHER_SANITIZER
# A leak fails the run (ASan), as an error does: the std and runtime leaks
# the first runs found are fixed (#3539), and so are the test programs that
# did not free what they made (#3547). SAN_LEAKS=report lists the leaks in
# the summary without failing, for a run that only surveys them.
leaks_fail=1
[ "${SAN_LEAKS:-fail}" = report ] && leaks_fail=0
case "$san" in
    ubsan)
        UBSAN_OPTIONS="print_stacktrace=1:report_error_type=1:log_exe_name=1:log_path=$reports/report"
        export UBSAN_OPTIONS
        ;;
    asan)
        # halt_on_error: the first error stops the program, as a UBSan report
        # does. allocator_may_return_null: a test that asks for more memory
        # than exists to see the refusal gets NULL, as without ASan, instead
        # of ASan's own abort (it writes a WARNING, which is not a report).
        # handle_segv/sigbus/sigfpe=0: a fault stays the signal it is, so a
        # program's own fault-to-panic handler and `ae run`'s crash report
        # see what they see without ASan; ASan's errors are found at the
        # access, not by catching the fault.
        # detect_odr_violation=0: a library built with --emit=lib carries
        # its own copy of the runtime, and a program that loads two of them
        # (or interposes its own, #3445) has the runtime's globals twice on
        # purpose; that is the design, not a one-definition mistake.
        # verify_asan_link_order=0: the sandbox runs a contained child with
        # its preload library first (LD_PRELOAD), which ASan otherwise
        # refuses to start under.
        # detect_leaks is left to each binary's default, not set here: on
        # Linux a program reports its leaks, while aetherc and ae turn theirs
        # off (compiler/aether_toolchain_asan.h), since aetherc's leaks are
        # gated shape by shape in check_compiler_leaks.sh (#1667). Their
        # memory errors still stop them. A program that leaks exits 23, so
        # the test that ran it fails; with SAN_LEAKS=report LeakSanitizer's
        # exit code is 0, and a leak alone fails no test.
        ASAN_OPTIONS="halt_on_error=1:allocator_may_return_null=1:handle_segv=0:handle_sigbus=0:handle_sigfpe=0:detect_odr_violation=0:verify_asan_link_order=0:log_exe_name=1:log_path=$reports/report"
        LSAN_OPTIONS="exitcode=$([ $leaks_fail = 1 ] && echo 23 || echo 0)"
        export ASAN_OPTIONS LSAN_OPTIONS
        ;;
    tsan)
        # halt_on_error: the first report stops the program (exit 66), so
        # the test that ran it fails, as under ASan and UBSan.
        # second_deadlock_stack: a lock-order inversion shows where each
        # thread took both of its locks, not only the second.
        # history_size=4: each thread remembers 512K accesses (the default,
        # 2, is 128K), so the other side of a race on a long-lived actor or
        # queue still has its stack instead of "failed to restore the
        # stack"; it costs a few MB a thread.
        # report_thread_leaks / report_signal_unsafe: TSan's defaults, named
        # so the run says what it checks: a thread that ended and was never
        # joined or detached, and a call in a signal handler that is not
        # async-signal-safe, are reports like a race.
        # allocator_may_return_null and handle_segv/sigbus/sigfpe=0, as for
        # ASan above: a refused allocation is NULL, and a fault stays the
        # signal the program's own handler expects.
        # suppressions: only code this repository does not own, each entry
        # with its reason (tests/tsan/suppressions.txt). A report in our code
        # is never suppressed.
        TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1:history_size=4:report_thread_leaks=1:report_signal_unsafe=1:allocator_may_return_null=1:handle_segv=0:handle_sigbus=0:handle_sigfpe=0:suppressions=$root/tests/tsan/suppressions.txt:log_exe_name=1:log_path=$reports/report"
        export TSAN_OPTIONS
        # TSan runs a program 5 to 15 times slower than a plain build. The
        # per-test limits are three times the sweep's and the C runner's
        # defaults, which still ends a hang, unless the caller set its own.
        AE_TEST_TIMEOUT=${AE_TEST_TIMEOUT:-360}
        AE_SH_TEST_TIMEOUT=${AE_SH_TEST_TIMEOUT:-1260}
        AE_C_TEST_TIMEOUT=${AE_C_TEST_TIMEOUT:-360}
        export AE_TEST_TIMEOUT AE_SH_TEST_TIMEOUT AE_C_TEST_TIMEOUT
        ;;
esac

# The part of the .ae corpus the run takes. Under ASan, the tests that check
# what ASan replaces (RSS, the sandbox's preload, the compiled code's shape,
# deep compiler recursion) are left out, each with its reason in
# tests/ae_sweep_prune_asan.txt. Under TSan, only the tests that put two
# threads on the same memory run (tests/tsan_sweep_select.txt), less the ones
# that check what TSan replaces (tests/ae_sweep_prune_tsan.txt). The select
# file is read now, so a mistake in it stops the run before the build.
prune=""
select=""
case "$san" in
    asan) prune="$root/tests/ae_sweep_prune_asan.txt" ;;
    tsan)
        prune="$root/tests/ae_sweep_prune_tsan.txt"
        select="$root/tests/tsan_sweep_select.txt"
        sh "$root/tests/scripts/sweep_select.sh" "$select" < /dev/null || exit 1
        ;;
esac

printf 'int main(void) { return 0; }\n' > "$dir/probe.c"
if ! "$CC" "$dir/probe.c" -o "$dir/probe" > "$dir/probe.log" 2>&1; then
    echo "test-$san: $CC cannot link a program with $flags here" >&2
    echo "  (no $name runtime: MinGW has none; run it on Linux or macOS)" >&2
    sed 's/^/  | /' "$dir/probe.log" >&2
    exit 1
fi
# And the runtime starts. TSan maps its shadow memory at fixed addresses,
# which fails under an address-space limit (ulimit -v) and, before LLVM 18,
# with the kernel's mmap randomization above 28 bits ("unexpected memory
# mapping"); every program would then die before main. The probe runs with
# no options file, so what it says comes here, not to a report.
if ! UBSAN_OPTIONS= ASAN_OPTIONS= TSAN_OPTIONS= "$dir/probe" > "$dir/probe.run.log" 2>&1; then
    echo "test-$san: a program linked with $flags does not start here" >&2
    if [ "$san" = tsan ]; then
        echo "  (TSan needs no ulimit -v, and before LLVM 18 vm.mmap_rnd_bits of 28 or less:" >&2
        echo "   sudo sysctl -w vm.mmap_rnd_bits=28)" >&2
    fi
    sed 's/^/  | /' "$dir/probe.run.log" >&2
    exit 1
fi

echo "== test-$san: the toolchain, built with $flags${tail_flags:+ (and $tail_flags last)}"
if ! "$make_cmd" -j"$nproc" compiler ae stdlib CC="$CC"; then
    # A report from anything the build runs stops the build too; show what
    # was reported, or the job ends with a build error and the reason only
    # in the uploaded reports.
    for f in "$reports"/report.*; do
        [ -f "$f" ] || continue
        echo "=== $f"
        head -80 "$f"
    done
    exit 1
fi

if ! nm "$root/build/libaether.a" 2>/dev/null | grep -q "$hook"; then
    echo "test-$san: build/libaether.a calls no $name hook ($hook): the flags did not reach the toolchain" >&2
    exit 1
fi

# canary <name> <what its report must say>: built by ae from tests/<san>/,
# run, and expected to stop with a report saying that.
canary() {
    c="$dir/canary/$1"
    if ! "$root/build/ae" build "tests/$san/$1.ae" --extra "tests/$san/canary.c" -o "$c" \
            > "$c.build.log" 2>&1; then
        echo "test-$san: the canary $1 did not build" >&2
        sed 's/^/  | /' "$c.build.log" >&2
        exit 1
    fi
    case "$san" in
        ubsan) UBSAN_OPTIONS="print_stacktrace=1:log_exe_name=1:log_path=$c.report" "$c" > "$c.out" 2>&1 ;;
        # The leak canary must fail by its leak, whatever SAN_LEAKS says.
        asan) ASAN_OPTIONS="detect_leaks=1:log_exe_name=1:log_path=$c.report" LSAN_OPTIONS="exitcode=23" \
                  "$c" > "$c.out" 2>&1 ;;
        tsan) TSAN_OPTIONS="halt_on_error=1:log_exe_name=1:log_path=$c.report" "$c" > "$c.out" 2>&1 ;;
    esac
    crc=$?
    if [ "$crc" -eq 0 ] || ! grep -q "$2" "$c".report.* 2>/dev/null; then
        echo "test-$san: the canary $1 was not stopped and reported (exit $crc, wanted '$2'):" >&2
        echo "  a program ae builds here is not instrumented, so a clean corpus would mean nothing" >&2
        exit 1
    fi
}
case "$san" in
    ubsan) canary canary 'shift exponent 32' ;;
    asan)
        canary overflow 'heap-buffer-overflow'
        canary leak 'detected memory leaks'
        ;;
    tsan) canary race 'WARNING: ThreadSanitizer: data race' ;;
esac
echo "  [PASS] libaether.a is instrumented, and the canary was stopped with a report"

status=0
case "${AE_SWEEP_SHARD:-}" in
    ""|1/*|1,*)
        echo "== test-$san: C unit tests"
        "$make_cmd" -j"$nproc" test CC="$CC" || status=1
        ;;
esac
echo "== test-$san: the .ae corpus (make test-ae${select:+, the tests ${select#"$root"/} picks}${AE_SWEEP_SHARD:+, shard $AE_SWEEP_SHARD})"
"$make_cmd" test-ae CC="$CC" NPROC="$nproc" AE_SWEEP_EXTRA_PRUNE="$prune" AE_SWEEP_SELECT="$select" || status=1

# Which report files are errors and which are leaks. A file holding only a
# WARNING (allocator_may_return_null's note on an impossible request) is
# neither.
errors=""
leaks=""
for f in "$reports"/report.*; do
    [ -f "$f" ] || continue
    if [ "$san" = ubsan ]; then
        grep -q 'runtime error:' "$f" && errors="$errors $f"
    elif [ "$san" = tsan ]; then
        # Every TSan report is a WARNING; a FATAL or CHECK failure is TSan
        # itself failing in that program, which is not a pass either.
        grep -q -E '(WARNING|ERROR|FATAL): ThreadSanitizer|ThreadSanitizer: CHECK failed' "$f" &&
            errors="$errors $f"
    elif grep -q 'ERROR: AddressSanitizer' "$f"; then
        errors="$errors $f"
    elif grep -q 'ERROR: LeakSanitizer' "$f"; then
        leaks="$leaks $f"
    fi
done

# table FILES...: the reports grouped by where they happened: the report
# line itself (UBSan), or the first stack frame that is not the sanitizer's,
# an allocator's or the generated program's own prelude (ASan, LSan), or the
# place TSan's SUMMARY names and, for a race, the first frame of the other
# access, so that two programs hitting one race land on one row (TSan).
table() {
    echo "| programs | where | what | programs, up to 4 |"
    echo "|---|---|---|---|"
    for f in "$@"; do
        exe=${f#"$reports"/report.}
        exe=${exe%.*}
        if [ "$san" = ubsan ]; then
            grep 'runtime error:' "$f" | head -1 | while IFS= read -r line; do
                loc=${line%%: runtime error:*}
                printf '%s\t%s\t%s\n' "$exe" "$loc" "${line#*: runtime error: }"
            done
        elif [ "$san" = tsan ]; then
            # "SUMMARY: ThreadSanitizer: data race /path/f.c:12:5 in fn": the
            # kind of report, then the top frame of the access it stopped at.
            sum=$(grep -m1 '^SUMMARY: ThreadSanitizer: ' "$f" | sed 's/^SUMMARY: ThreadSanitizer: //')
            [ -n "$sum" ] || sum=$(grep -m1 'ThreadSanitizer' "$f")
            case "$sum" in
                *" in "*)
                    what=$(printf '%s\n' "$sum" | sed -E 's/^(.*) [^ ]+ in [^ ]+$/\1/')
                    loc=$(printf '%s\n' "$sum" | sed -E 's/^.* ([^ ]+ in [^ ]+)$/\1/')
                    ;;
                *) what=$sum; loc="(no frame)" ;;
            esac
            # "  Previous write of size 4 at ... by thread T2:", then its
            # frames ("#0 fn /path/f.c:40:3 (prog+0x...)").
            prev=$(awk '/^  Previous /{ p = 1; next } p && /^ *#[0-9]+ /{ print $3 " in " $2; exit }' "$f")
            [ -n "$prev" ] && loc="$loc, against $prev"
            printf '%s\t%s\t%s\n' "$exe" "$loc" "$what"
        else
            what=$(grep -m1 -E 'ERROR: (Address|Leak)Sanitizer' "$f" | sed 's/^.*Sanitizer: //; s/ on address.*//')
            loc=$(grep -E '^ *#[0-9]+ .* in ' "$f" | sed 's/^ *#[0-9]* 0x[0-9a-f]* in //' |
                  grep -v -E 'libsanitizer|/usr/include/|libc\.so|aether_resource_caps\.c|_aether_alloc|string_alloc_inline|^(malloc|calloc|realloc|strdup) ' |
                  grep -v -E ' (/tmp/[^ ]*|build/[^ ]*)\.c:' | head -1)
            [ -n "$loc" ] || loc=$(grep -m1 -E '^ *#[0-9]+ ' "$f" | sed 's/^.* in //')
            printf '%s\t%s\t%s\n' "$exe" "$loc" "$what"
        fi
    done | awk -F'\t' -v root="$root/" '{
            loc = $2
            gsub(root, "", loc)
            msg = $3; gsub(/\|/, "/", msg)
            if (!((loc, $1) in seen)) {
                seen[loc, $1] = 1; c[loc]++
                if (c[loc] <= 4) ex[loc] = ex[loc] " " $1
            }
            if (!(loc in first)) first[loc] = msg
        }
        END { for (l in c) printf "| %d | %s | %s |%s |\n", c[l], l, first[l], ex[l] }' |
        sort -t'|' -k2,2nr
}

ne=$(printf '%s\n' $errors | grep -c . || true)
nl=$(printf '%s\n' $leaks | grep -c . || true)
{
    echo "## The C unit tests and the .ae corpus${select:+ (the part ${select#"$root"/} picks)} under $name"
    echo
    echo "$ne program run(s) stopped by $name."
    if [ "$ne" -gt 0 ]; then echo; table $errors; fi
    if [ "$san" = asan ]; then
        echo
        if [ "$leaks_fail" = 1 ]; then
            echo "$nl program run(s) that leaked, which fails the run."
        else
            echo "$nl program run(s) that leaked, listed only (SAN_LEAKS=report)."
        fi
        if [ "$nl" -gt 0 ]; then echo; table $leaks; fi
    fi
} > "$dir/summary.md"
cat "$dir/summary.md"
[ -n "${GITHUB_STEP_SUMMARY:-}" ] && cat "$dir/summary.md" >> "$GITHUB_STEP_SUMMARY"
if [ "$ne" -gt 0 ]; then
    # A TSan report holds two stacks and where each thread was started.
    lines=40
    [ "$san" = tsan ] && lines=80
    for f in $(printf '%s\n' $errors | head -20); do
        echo "=== $f"
        head -"$lines" "$f"
    done
    echo "test-$san: $ne program run(s) were stopped by $name; the reports are in build/$san/reports" >&2
    status=1
fi
if [ "$nl" -gt 0 ] && [ "$leaks_fail" = 1 ]; then
    echo "test-$san: $nl program run(s) leaked; the reports are in build/$san/reports" >&2
    status=1
fi
exit "$status"
