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
#
# FORTIFY is checked by running an overflow into it rather than by reading a
# symbol name, because the name is a libc implementation detail and the
# protection is not: glibc and Apple libc call out to __*_chk, mingw-w64
# inlines the same check and leaves no symbol behind. Behaviour is the one
# question that has the same answer on all three, so the overflow is the
# check, on every platform, with no exception for the one that reads
# differently.

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

extern probe_copy(n: int) -> int

fill(n: int) -> long {
    byte[64] scratch
    mem.set_long(scratch, 0, n * 3)
    return mem.get_long(scratch, 0)
}

main() {
    probe_copy(8)
    println("v=${fill(7)}")
}
AEOF

cat > "$TMP/overflow.ae" <<'AEOF'
extern probe_copy(n: int) -> int

main() {
    probe_copy(32)
    println("overflow not caught")
}
AEOF

# One C function, compiled by `ae build` with the same flags as the Aether
# code, called with a safe length by one probe and an overflowing one by the
# other. Going through the libc header with a destination the compiler can
# size is what makes the call fortifiable at all, and the length arriving from
# the caller is what keeps the check at runtime instead of folded away.
#
# 32 bytes into a 16-byte object overruns the whole object, which is what both
# _FORTIFY_SOURCE levels bound, so every libc here catches it. The destination
# is static rather than on the stack, so a stack canary cannot be what catches
# it: that leaves one explanation for the process dying.
cat > "$TMP/probe_copy.c" <<'COF'
#include <string.h>

static char small[16];
static char slack[256];

int probe_copy(int n) {
    const char* src = "0123456789abcdefghijklmnopqrstuv";
    memcpy(small, src, (size_t)n);
    slack[0] = small[0];
    return small[0] + slack[0];
}
COF

if ! "$AE" build "$TMP/probe.ae" --extra "$TMP/probe_copy.c" -o "$TMP/probe" \
        >"$TMP/build.log" 2>&1; then
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

# The canary is read from a name, and a PE need not carry a symbol table for
# it to be read from; where there is nothing to read, checksec says n/a rather
# than reporting an absence it did not observe. ELF and Mach-O always name it.
fmt="$(printf '%s\n' "$report" | head -1)"
canary="$(printf '%s\n' "$report" | grep -E "^  canary " | awk '{print $2}')"
case "$fmt" in
    *PE*) case "$canary" in
              yes|n/a) ;;
              *) fail "canary is '$canary' on PE, expected yes or n/a" ;;
          esac ;;
    *)    [ "$canary" = "yes" ] || fail "canary is '$canary' on $fmt, expected yes" ;;
esac

# RELRO is an ELF concept; Mach-O and PE report n/a rather than a failure.
relro="$(printf '%s\n' "$report" | grep -E "^  RELRO " | awk '{print $2}')"
case "$relro" in
    yes|n/a) ;;
    *) fail "RELRO is '$relro', expected full (yes) on ELF or n/a elsewhere" ;;
esac

# FORTIFY, proven by behaviour: the overflow must not complete. A fortified
# build dies on the bound check (SIGABRT under glibc and mingw, SIGTRAP under
# Apple libc, all of them non-zero); an unfortified one writes past the object
# and prints. Both halves are asserted, because a process that dies for some
# other reason would otherwise look like a pass.
if ! "$AE" build "$TMP/overflow.ae" --extra "$TMP/probe_copy.c" -o "$TMP/overflow" \
        >"$TMP/overflow_build.log" 2>&1; then
    sed 's/^/        /' "$TMP/overflow_build.log" | head -10
    fail "the overflow probe did not build"
fi
OVERFLOW="$(sed -n 's/^Built[^:]*: //p' "$TMP/overflow_build.log" | tail -1)"
[ -n "$OVERFLOW" ] || fail "the overflow build printed no artifact path"

overflow_out="$("$OVERFLOW" 2>&1)"
overflow_rc=$?
[ "$overflow_rc" -ne 0 ] \
    || fail "the overflow completed with exit 0: FORTIFY did not catch a 32-byte write into a 16-byte object"
case "$overflow_out" in
    *"overflow not caught"*)
        fail "the overflow ran to completion (output: $overflow_out): FORTIFY is not in effect" ;;
esac

# And what checksec reads back agrees. glibc and Apple libc leave a __*_chk
# call to find; mingw-w64 inlines the check, so a PE with no symbol table has
# nothing to read and says n/a rather than guessing "no". The run above is
# what proves the protection there.
fortify="$(printf '%s\n' "$report" | grep -E "^  FORTIFY " | awk '{print $2}')"
case "$fmt" in
    *PE*) case "$fortify" in
              yes|n/a) ;;
              *) fail "FORTIFY is '$fortify' on PE, expected yes or n/a" ;;
          esac ;;
    *)    [ "$fortify" = "yes" ] || fail "FORTIFY is '$fortify' on $fmt, expected yes" ;;
esac

# The gate agrees with the report.
"$AE" checksec --require "pie,nx,canary,fortify" "$PROBE" >/dev/null 2>&1 \
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
