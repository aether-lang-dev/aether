#!/bin/sh
# `ae build` produces hardened binaries, and `ae checksec` can prove it (#1646).
#
# Two things this pins, because both were absent and neither was visible:
#   - a program built by `ae build` carries the mitigations the platform
#     supports: PIE, non-executable stack, full RELRO where the format has it,
#     a stack canary, and fortified libc calls;
#   - `ae checksec --require` exits non-zero when one is missing, which is the
#     part that keeps a flag change from quietly dropping a mitigation.
#
# The probe deliberately contains a stack buffer: -fstack-protector-strong
# protects functions that have one, so a program without any would report no
# canary and prove nothing.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] checksec_hardening: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

cat > "$TMP/probe.ae" <<'AEOF'
import std.mem

fill(n: int) -> long {
    byte[64] scratch
    mem.set_long(scratch, 0, n * 3)
    return mem.get_long(scratch, 0)
}

main() {
    println("v=${fill(7)}")
}
AEOF

if ! "$AE" build "$TMP/probe.ae" -o "$TMP/probe" >"$TMP/build.log" 2>&1; then
    sed 's/^/        /' "$TMP/build.log" | head -10
    fail "the probe did not build"
fi

# Take the artifact's path from the build's own report. Windows appends .exe
# whatever -o asked for, and the filesystem cannot be asked about it from here:
# MSYS2's `test -f probe` answers yes for probe.exe, so the shell sees a file
# that the native `ae` binary, going through the Win32 CRT, does not. `ae
# build` prints the path it actually wrote ("Built: <path>", or "Built (cache
# hit): <path>"), which is the one thing that is true on every platform.
PROBE="$(sed -n 's/^Built[^:]*: //p' "$TMP/build.log" | tail -1)"
[ -n "$PROBE" ] || { sed 's/^/        /' "$TMP/build.log" | head -10
                     fail "the build printed no artifact path"; }

# Hardening must not have broken the program.
[ "$("$PROBE")" = "v=21" ] || fail "the hardened probe printed '$("$PROBE")', expected 'v=21'"

report="$("$AE" checksec "$PROBE" 2>&1)" || fail "ae checksec failed: $report"
printf '%s' "$report" | grep -q "PIE" || fail "checksec printed no report: $report"

check_prop() {   # check_prop <label> <expected-word>
    line="$(printf '%s\n' "$report" | grep -E "^  $1 ")" || fail "checksec reported no $1 row"
    word="$(printf '%s' "$line" | awk '{print $2}')"
    [ "$word" = "$2" ] || fail "$1 is '$word', expected '$2' (full report: $line)"
}

# Every platform the toolchain targets gets these.
check_prop PIE yes
check_prop NX yes
check_prop canary yes

# RELRO is an ELF concept; Mach-O and PE report n/a rather than a failure.
relro="$(printf '%s\n' "$report" | grep -E "^  RELRO " | awk '{print $2}')"
case "$relro" in
    yes|n/a) ;;
    *) fail "RELRO is '$relro', expected full (yes) on ELF or n/a elsewhere" ;;
esac

# FORTIFY is asked for on every platform, but whether an artifact ends up with
# a __*_chk call is the libc's decision: glibc's headers redirect the
# printf-family calls this probe makes, Apple's do not for the same source. So
# require it where it is observable and record what was seen elsewhere, rather
# than assert a platform difference away.
fortify="$(printf '%s\n' "$report" | grep -E "^  FORTIFY " | awk '{print $2}')"
fmt="$(printf '%s\n' "$report" | head -1)"
case "$fmt" in
    *ELF*) [ "$fortify" = "yes" ] || fail "FORTIFY is '$fortify' on ELF, expected yes" ;;
    *)     : ;;
esac

# The gate agrees with the report.
require="pie,nx,canary"
[ "$fortify" = "yes" ] && require="$require,fortify"
"$AE" checksec --require "$require" "$PROBE" >/dev/null 2>&1 \
    || fail "--require rejected a binary the report says is hardened"

# And the gate is a gate: something the binary cannot have must fail it.
if "$AE" checksec --require stripped "$PROBE" >/dev/null 2>&1; then
    fail "--require stripped passed on an unstripped binary, so the gate proves nothing"
fi

# A file that is not a binary is an error, not a silent pass.
echo "not a binary" > "$TMP/text"
if "$AE" checksec "$TMP/text" >/dev/null 2>&1; then
    fail "checksec accepted a text file"
fi

echo "  [PASS] checksec_hardening: ae build hardens, ae checksec proves it, --require gates it"
