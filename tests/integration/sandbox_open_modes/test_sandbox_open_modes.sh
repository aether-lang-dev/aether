#!/bin/sh
# Under the LD_PRELOAD library, an open is checked for everything it asks of
# the file (#3742). "r+" reads and writes, so a read-only grant must refuse
# it; it used to be checked as a read and the file could be written. "w+",
# "a+" and O_RDWR read as well as write, so a write-only grant refuses them.
# freopen, which glibc opens on its internal open, gets fopen's rule, and so
# does a freopen with no path, which glibc reopens through /proc/self/fd/N.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LIB="$ROOT/build/libaether_sandbox.so"

os="$(uname -s 2>/dev/null)"
case "$os" in
    Linux|FreeBSD) ;;
    *)
        echo "  [SKIP] sandbox_open_modes: preload is Linux and FreeBSD only"
        exit 0 ;;
esac
if [ ! -f "$LIB" ]; then
    echo "  [SKIP] sandbox_open_modes: preload library not built"
    exit 0
fi

TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT
if ! ${CC:-cc} -std=c11 -Wall -Wextra -Werror \
        "$SCRIPT_DIR/harness.c" -o "$TMP/harness"; then
    echo "  [FAIL] sandbox_open_modes: harness did not compile"
    exit 1
fi

DATA="$TMP/data.txt"
printf 'open modes sentinel\n' > "$DATA"
printf 'fs_read:%s\n' "$DATA" > "$TMP/read.grants"
printf 'fs_write:%s\n' "$DATA" > "$TMP/write.grants"
printf 'fs_read:%s\nfs_write:%s\n' "$DATA" "$DATA" > "$TMP/both.grants"
fail=0

# expected: 0 (opened) or 3 (refused with EACCES, the sandbox's refusal).
# A third value is accepted only where a case says so.
run_case() {
    label="$1"
    grants="$2"
    case_name="$3"
    expected="$4"
    also="${5:-}"
    LD_PRELOAD="$LIB" AETHER_SANDBOX_GRANTS="$TMP/$grants.grants" \
        AETHER_SANDBOX_LOG=none "$TMP/harness" "$case_name" "$DATA" \
        </dev/null >"$TMP/out" 2>"$TMP/err"
    got=$?
    if [ "$got" -eq "$expected" ] || { [ -n "$also" ] && [ "$got" -eq "$also" ]; }; then
        echo "  [PASS] $label"
    else
        echo "  [FAIL] $label: exit $got, expected $expected"
        sed 's/^/    /' "$TMP/err"
        fail=1
    fi
}

run_case "read grant: fopen r opens"              read  fopen-r      0
run_case "read grant: fopen r+ is refused"        read  fopen-r+     3
run_case "read grant: fopen rb+ is refused"       read  fopen-rb+    3
run_case "read grant: fopen a is refused"         read  fopen-a      3
run_case "read grant: freopen r+ is refused"      read  freopen-r+   3
run_case "read grant: open O_RDONLY opens"        read  open-rdonly  0
run_case "read grant: open O_RDWR is refused"     read  open-rdwr    3
run_case "write grant: fopen a opens"             write fopen-a      0
run_case "write grant: fopen w+ is refused"       write fopen-w+     3
run_case "write grant: open O_WRONLY opens"       write open-wronly  0
run_case "write grant: open O_RDWR is refused"    write open-rdwr    3
run_case "both grants: fopen r+ opens"            both  fopen-r+     0
run_case "both grants: freopen r+ opens"          both  freopen-r+   0
run_case "both grants: open O_RDWR opens"         both  open-rdwr    0

# A freopen with no path, from a read-only stream to "r+". glibc reopens
# the file through /proc/self/fd/N, which the read grant must not let
# write. FreeBSD's libc only changes the descriptor's flags and refuses to
# widen its access itself (EBADF, exit 4), with any grants.
if [ "$os" = FreeBSD ]; then
    run_case "read grant: freopen(NULL, r+) is refused" read freopen-null-r+ 3 4
    run_case "both grants: freopen(NULL, r+) is libc's to refuse" both freopen-null-r+ 4
else
    run_case "read grant: freopen(NULL, r+) is refused" read freopen-null-r+ 3
    run_case "both grants: freopen(NULL, r+) opens" both freopen-null-r+ 0
fi

# freopen(NULL, "rb", stdin) asks for no more than stdin holds, so it needs
# no grant: stdin here is /dev/null (above) or a pipe, whose
# /proc/self/fd link names nothing a grant matches. It was refused, and
# stdin closed with it.
run_case "read grant: freopen(NULL, rb, stdin) on /dev/null keeps it" read freopen-null-stdin-rb 0
printf 'piped\n' | LD_PRELOAD="$LIB" AETHER_SANDBOX_GRANTS="$TMP/read.grants" \
    AETHER_SANDBOX_LOG=none "$TMP/harness" freopen-null-stdin-rb "$DATA" >"$TMP/out" 2>"$TMP/err"
got=$?
if [ "$got" -eq 0 ]; then
    echo "  [PASS] read grant: freopen(NULL, rb, stdin) on a pipe keeps it"
else
    echo "  [FAIL] read grant: freopen(NULL, rb, stdin) on a pipe: exit $got, expected 0"
    sed 's/^/    /' "$TMP/err"
    fail=1
fi

if ! grep -qF 'open modes sentinel' "$DATA"; then
    echo "  [FAIL] the file was changed by an open that was refused"
    fail=1
fi

echo ""
echo "sandbox_open_modes: $([ "$fail" -eq 0 ] && echo PASS || echo FAIL)"
[ "$fail" -eq 0 ]
