#!/bin/sh
# std.posix.regex against the host's own regcomp/regexec (#3652).
#
# std/posix/regex/tests/conformance.txt holds POSIX cases with the answers
# FreeBSD's engine gives. test_posix_regex.ae checks those answers on every
# target; this checks them against an independent implementation too:
# system_regex_oracle.c runs each case through the host's <regex.h> in a UTF-8
# locale, posix_regex_cases.ae runs it through std.posix.regex, and the two
# outputs must be identical.
#
# Hosts: FreeBSD (the same engine, so any difference is ours) and macOS (an
# independent TRE-derived engine). glibc and musl are skipped: they disagree
# with POSIX/FreeBSD on cases the table keeps on purpose ([[.hyphen.]]
# collating names, ranges of non-ASCII letters), and Windows has no
# <regex.h>, which is why the module exists.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
CASES="$ROOT/std/posix/regex/tests/conformance.txt"

case "$(uname -s)" in
    Darwin|FreeBSD) ;;
    *)
        echo "  [PASS] posix_regex_system: SKIP (compared only against FreeBSD and macOS regcomp)"
        exit 0
        ;;
esac

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT
CC="${CC:-cc}"

fail() {
    echo "  [FAIL] posix_regex_system: $1"
    shift
    for f in "$@"; do
        [ -f "$f" ] && sed 's/^/    /' "$f" | tail -30
    done
    exit 1
}

"$CC" -o "$TMPDIR/oracle" "$SCRIPT_DIR/system_regex_oracle.c" >"$TMPDIR/cc.log" 2>&1 \
    || fail "building the system-regcomp oracle" "$TMPDIR/cc.log"
"$TMPDIR/oracle" "$CASES" >"$TMPDIR/system.txt" 2>"$TMPDIR/oracle.err" \
    || fail "the oracle failed" "$TMPDIR/oracle.err"

( cd "$TMPDIR" && "$ROOT/build/ae" build "$SCRIPT_DIR/posix_regex_cases.ae" -o "$TMPDIR/cases" ) \
    >"$TMPDIR/build.log" 2>&1 || fail "building posix_regex_cases.ae" "$TMPDIR/build.log"
"$TMPDIR/cases" "$CASES" >"$TMPDIR/aether.txt" 2>"$TMPDIR/run.err" \
    || fail "posix_regex_cases failed" "$TMPDIR/run.err"

n="$(wc -l <"$TMPDIR/system.txt" | tr -d ' ')"
[ "$n" -gt 50 ] || fail "the oracle printed only $n cases" "$TMPDIR/system.txt"

. "$ROOT/tests/lib/same_bytes.sh"
if ! same_bytes "$TMPDIR/system.txt" "$TMPDIR/aether.txt"; then
    if command -v diff >/dev/null 2>&1; then
        diff "$TMPDIR/system.txt" "$TMPDIR/aether.txt" >"$TMPDIR/diff.txt"
    fi
    fail "std.posix.regex and $(uname -s)'s regcomp disagree (< system, > std.posix.regex; numbers are conformance.txt lines)" "$TMPDIR/diff.txt"
fi
echo "  [PASS] posix_regex_system: $n cases agree with $(uname -s)'s regcomp"
