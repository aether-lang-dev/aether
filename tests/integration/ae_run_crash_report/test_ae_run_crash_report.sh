#!/bin/sh
# `ae run` names what a crashed program died of, on every platform.
#
# run_cmd_forwarding returns the negated signal on POSIX; on Windows the
# process exit code, which for a crash is the NTSTATUS the OS terminated it
# with (0xC0000005 for an access violation). Both are negative as an int,
# and the report used to print the negation of either as a "signal": on
# Windows that was `signal 1073741819`, a number that means nothing. The
# report now names the cause on both.
#
# The crash is a read through address 8: it faults on every platform. An
# integer divide by zero would not do -- ARM64 returns 0 for it.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] ae_run_crash_report: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/fault.ae" <<'AE'
import std.mem
main() {
    p = mem.long_to_ptr(8)
    println("${mem.get_int(p, 0)}")
}
AE

out="$("$AE" run "$tmp/fault.ae" 2>&1)"
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "  [FAIL] ae_run_crash_report: a read through address 8 exited 0"
    exit 1
fi
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        want="0xC0000005: access violation" ;;
    Darwin)
        # macOS raises SIGBUS or SIGSEGV for this depending on the page.
        want="signal 1[01]: (bus error|segmentation fault)" ;;
    *)
        want="signal 11: segmentation fault" ;;
esac
if printf '%s' "$out" | grep -Eq "Program crashed \($want\)"; then
    echo "  [PASS] ae_run_crash_report: the fault is reported as '$want'"
else
    echo "  [FAIL] ae_run_crash_report: expected 'Program crashed ($want)'; got:"
    printf '%s\n' "$out" | grep -i "crash" | head -3 | sed 's/^/    /'
    exit 1
fi
