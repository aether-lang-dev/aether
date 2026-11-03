#!/bin/sh
# `make test-ubsan` (#2488) and `make test-asan-ae` (#3539): the C unit tests
# and the `make test-ae` sweep, with the toolchain, libaether and every
# program they build compiled under a sanitizer. Run from the repository root.
#
#   SAN=ubsan MAKE=make NPROC=4 BASE_CC=gcc sh tests/scripts/sanitizer_sweep.sh
#   SAN=asan  MAKE=make NPROC=4 BASE_CC=gcc sh tests/scripts/sanitizer_sweep.sh
#
# SAN=ubsan is UndefinedBehaviorSanitizer; SAN=asan is AddressSanitizer with
# LeakSanitizer, so a heap overflow, a use after free or a double free stops
# the program, and a leak is reported when it exits.
#
# Every compile and link goes through the sanitizing compiler: build/<san>/bin
# holds a `gcc` and a `cc` that run the real ones with the sanitizer's flags,
# put first on PATH and named by absolute path in CC. That reaches what the
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
# each in its own run). Without them a run whose flags were lost would read
# as a clean corpus.
#
# Needs a C compiler with the sanitizer's runtime: gcc's libubsan / libasan or
# clang's compiler-rt (Linux, macOS). MinGW has none; there it stops and says
# so. AE_SWEEP_SHARD=k/n splits the .ae corpus as it does for `make test-ae`;
# the C unit tests run in shard 1 only.
set -u

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
    *)
        echo "sanitizer_sweep.sh: SAN must be ubsan or asan, got '$san'" >&2
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
for cc_name in gcc cc; do
    real=$(PATH=$plain_path command -v "$cc_name" 2>/dev/null) || continue
    case "$real" in /*) ;; *) continue ;; esac
    printf '#!/bin/sh\n# %s under %s, written by tests/scripts/sanitizer_sweep.sh.\nexec "%s" %s "$@"\n' \
        "$cc_name" "$name" "$real" "$flags" > "$bin/$cc_name"
    chmod +x "$bin/$cc_name"
done
case "$base_cc" in gcc|cc) ;; *) base_cc=cc ;; esac
if [ ! -x "$bin/$base_cc" ]; then
    echo "test-$san: no $base_cc on PATH to wrap" >&2
    exit 1
fi
CC="$bin/$base_cc"
PATH="$bin:$PATH"
export CC PATH
case "$san" in
    ubsan)
        UBSAN_OPTIONS="print_stacktrace=1:report_error_type=1:log_exe_name=1:log_path=$reports/report"
        export UBSAN_OPTIONS
        ;;
    asan)
        # halt_on_error: the first error stops the program, as a UBSan report
        # does. allocator_may_return_null: a test that asks for more memory
        # than exists to see the refusal gets NULL, as without ASan, instead
        # of ASan's own abort. LeakSanitizer reports at exit into the same
        # log, and its nonzero exit fails the test that ran the program.
        # detect_leaks is left to each binary's default, not set here: on
        # Linux a program reports its leaks, while aetherc and ae turn theirs
        # off (compiler/aether_toolchain_asan.h), since aetherc's leaks are
        # gated shape by shape in check_compiler_leaks.sh (#1667). Their
        # memory errors still stop them.
        ASAN_OPTIONS="halt_on_error=1:allocator_may_return_null=1:log_exe_name=1:log_path=$reports/report"
        export ASAN_OPTIONS
        ;;
esac

printf 'int main(void) { return 0; }\n' > "$dir/probe.c"
if ! "$CC" "$dir/probe.c" -o "$dir/probe" > "$dir/probe.log" 2>&1; then
    echo "test-$san: $CC cannot link a program with $flags here" >&2
    echo "  (no $name runtime: MinGW has none; run it on Linux or macOS)" >&2
    sed 's/^/  | /' "$dir/probe.log" >&2
    exit 1
fi

echo "== test-$san: the toolchain, built with $flags"
"$make_cmd" -j"$nproc" compiler ae stdlib CC="$CC" || exit 1

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
        asan) ASAN_OPTIONS="detect_leaks=1:log_exe_name=1:log_path=$c.report" "$c" > "$c.out" 2>&1 ;;
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
esac
echo "  [PASS] libaether.a is instrumented, and the canary was stopped with a report"

status=0
case "${AE_SWEEP_SHARD:-}" in
    ""|1/*|1,*)
        echo "== test-$san: C unit tests"
        "$make_cmd" -j"$nproc" test CC="$CC" || status=1
        ;;
esac
echo "== test-$san: the .ae corpus (make test-ae${AE_SWEEP_SHARD:+, shard $AE_SWEEP_SHARD})"
"$make_cmd" test-ae CC="$CC" NPROC="$nproc" || status=1

# The reports, grouped by the source location of the first frame in this
# tree (ASan, LSan) or of the report line itself (UBSan).
n=$(ls "$reports" | wc -l | tr -d ' ')
{
    echo "## The C unit tests and the .ae corpus under $name"
    echo
    echo "$n report file(s), one per program run that was stopped or leaked."
    if [ "$n" -gt 0 ]; then
        echo
        echo "| programs | where | what | programs, up to 4 |"
        echo "|---|---|---|---|"
        for f in "$reports"/report.*; do
            exe=${f#"$reports"/report.}
            exe=${exe%.*}
            if [ "$san" = ubsan ]; then
                grep 'runtime error:' "$f" | head -1 | while IFS= read -r line; do
                    loc=${line%%: runtime error:*}
                    printf '%s\t%s\t%s\n' "$exe" "$loc" "${line#*: runtime error: }"
                done
            else
                what=$(grep -m1 -E 'ERROR: (Address|Leak)Sanitizer' "$f" | sed 's/^.*Sanitizer: //; s/ on address.*//')
                loc=$(grep -E '^ *#[0-9]+ ' "$f" | grep -F "$root/" | grep -v -e '/tests/' | head -1 |
                      sed 's/^.* in [^ ]* //')
                [ -n "$loc" ] || loc=$(grep -m1 -E '^ *#[0-9]+ ' "$f" | sed 's/^.* in //')
                printf '%s\t%s\t%s\n' "$exe" "$loc" "$what"
            fi
        done | awk -F'\t' -v root="$root/" '{
                loc = $2
                if (index(loc, root) == 1) loc = substr(loc, length(root) + 1)
                msg = $3; gsub(/\|/, "/", msg)
                if (!((loc, $1) in seen)) {
                    seen[loc, $1] = 1; c[loc]++
                    if (c[loc] <= 4) ex[loc] = ex[loc] " " $1
                }
                if (!(loc in first)) first[loc] = msg
            }
            END { for (l in c) printf "| %d | %s | %s |%s |\n", c[l], l, first[l], ex[l] }' |
            sort -t'|' -k2,2nr
    fi
} > "$dir/summary.md"
cat "$dir/summary.md"
[ -n "${GITHUB_STEP_SUMMARY:-}" ] && cat "$dir/summary.md" >> "$GITHUB_STEP_SUMMARY"
if [ "$n" -gt 0 ]; then
    for f in $(ls "$reports"/report.* | head -20); do
        echo "=== $f"
        head -40 "$f"
    done
    echo "test-$san: $n program run(s) were stopped or leaked; the reports are in build/$san/reports" >&2
    status=1
fi
exit "$status"
