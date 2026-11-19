#!/bin/sh
# A permanent policy under the LD_PRELOAD library is held by the kernel too
# (#3751). A grant file with the line "policy:permanent" makes the
# preload's constructor apply Landlock to the whole process from its fs
# grants, before main: an openat issued as a bare syscall instruction, which
# no libc wrapper sees, is then refused for a file outside the grants. A
# grant file without the line keeps today's behaviour (the bare syscall is
# not held), and a grant Landlock cannot express (a suffix such as "*.log")
# leaves the policy at libc, saying so in the log. On a kernel without
# Landlock the log says that, and the kernel cases skip.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LIB="$ROOT/build/libaether_sandbox.so"

case "$(uname -s 2>/dev/null)" in
    Linux) ;;
    *)
        echo "  [SKIP] sandbox_preload_landlock: Landlock is Linux"
        exit 0 ;;
esac
case "$(uname -m 2>/dev/null)" in
    x86_64|aarch64) ;;
    *)
        echo "  [SKIP] sandbox_preload_landlock: the probe's bare syscall is x86_64 and aarch64"
        exit 0 ;;
esac
if [ ! -f "$LIB" ]; then
    echo "  [SKIP] sandbox_preload_landlock: preload library not built"
    exit 0
fi

TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT
if ! ${CC:-cc} -std=c11 -Wall -Wextra -Werror "$SCRIPT_DIR/probe.c" -o "$TMP/probe"; then
    echo "  [FAIL] sandbox_preload_landlock: probe did not compile"
    exit 1
fi

mkdir -p "$TMP/box" "$TMP/outside"
printf 'inside\n' > "$TMP/box/inside.txt"
printf 'outside\n' > "$TMP/outside/secret.txt"
# The probe itself is exec'd before the policy exists; it reads nothing else.
printf 'fs_read:%s/*\npolicy:permanent\n' "$TMP/box" > "$TMP/permanent.grants"
printf 'fs_read:%s/*\n' "$TMP/box" > "$TMP/plain.grants"
printf 'fs_read:%s/*\nfs_read:*.log\npolicy:permanent\n' "$TMP/box" > "$TMP/suffix.grants"
fail=0

# run GRANTS MODE FILE: the probe's exit status, its log on stderr.
run() {
    LD_PRELOAD="$LIB" AETHER_SANDBOX_GRANTS="$TMP/$1.grants" AETHER_SANDBOX_LOG=stderr \
        "$TMP/probe" "$2" "$3" </dev/null >"$TMP/out" 2>"$TMP/err"
}

expect() {
    label="$1"
    got="$2"
    want="$3"
    if [ "$got" -eq "$want" ]; then
        echo "  [PASS] $label"
    else
        echo "  [FAIL] $label: exit $got, expected $want"
        sed 's/^/    /' "$TMP/err"
        fail=1
    fi
}

run permanent raw "$TMP/outside/secret.txt"
got=$?
if grep -q 'landlock: this kernel has none' "$TMP/err"; then
    echo "  [SKIP] sandbox_preload_landlock: this kernel has no Landlock (the log says so)"
    exit 0
fi
if [ "$got" -eq 7 ]; then
    echo "  [SKIP] sandbox_preload_landlock: no bare syscall here"
    exit 0
fi
expect "permanent: a bare openat outside the grants is refused by the kernel" "$got" 3
grep -q 'landlock: the permanent policy is held by the kernel too' "$TMP/err" ||
    { echo "  [FAIL] permanent: the log does not say the kernel holds it"; fail=1; }

run permanent raw "$TMP/box/inside.txt"
expect "permanent: a bare openat of a granted file opens" "$?" 0
run permanent libc "$TMP/box/inside.txt"
expect "permanent: open() of a granted file opens" "$?" 0
run permanent libc "$TMP/outside/secret.txt"
expect "permanent: open() outside the grants is refused" "$?" 3

run plain raw "$TMP/outside/secret.txt"
expect "no permanent line: the bare openat is not held, as before" "$?" 0

run suffix raw "$TMP/outside/secret.txt"
expect "a grant Landlock cannot express: the policy stays at libc" "$?" 0
grep -q "landlock: the fs_read grant '\*.log' cannot be a Landlock rule" "$TMP/err" ||
    { echo "  [FAIL] suffix: the log does not say why the kernel does not hold it"; fail=1; }

# An exec grant under the kernel's hold: the program starts (Landlock needs
# EXECUTE on its ELF interpreter too, and on a "#!" script's interpreter),
# and keeps the preload and its grants (the library and the grant file are
# readable to it), so its own log says the policy is held again.
LS="$(command -v ls)"
case "$LS" in
    /*)
        printf 'fs_read:%s/*\nfs_read:%s\nexec:%s\npolicy:permanent\n' "$TMP/box" "$TMP/box" "$LS" \
            > "$TMP/exec.grants"
        LD_PRELOAD="$LIB" AETHER_SANDBOX_GRANTS="$TMP/exec.grants" AETHER_SANDBOX_LOG=stderr \
            "$TMP/probe" exec "$LS" "$TMP/box" </dev/null >"$TMP/out" 2>"$TMP/err"
        got=$?
        expect "permanent with exec:$LS: the granted program starts" "$got" 0
        grep -q 'inside.txt' "$TMP/out" ||
            { echo "  [FAIL] exec: ls did not list the box"; sed 's/^/    /' "$TMP/err"; fail=1; }
        held=$(grep -c 'landlock: the permanent policy is held by the kernel too' "$TMP/err")
        if [ "$held" -ge 2 ]; then
            echo "  [PASS] the started program kept the preload and its grants"
        else
            echo "  [FAIL] the started program did not keep the preload (the policy was held $held time(s))"
            sed 's/^/    /' "$TMP/err"
            fail=1
        fi ;;
    *) echo "  [SKIP] exec: no ls on PATH" ;;
esac
if [ -x /bin/sh ]; then
    printf '#!/bin/sh\nexit 5\n' > "$TMP/box/run.sh"
    chmod 755 "$TMP/box/run.sh"
    printf 'fs_read:%s/*\nexec:%s/run.sh\npolicy:permanent\n' "$TMP/box" "$TMP/box" > "$TMP/script.grants"
    LD_PRELOAD="$LIB" AETHER_SANDBOX_GRANTS="$TMP/script.grants" AETHER_SANDBOX_LOG=stderr \
        "$TMP/probe" exec "$TMP/box/run.sh" </dev/null >"$TMP/out" 2>"$TMP/err"
    expect "permanent with a #! script's exec grant: the script runs under its interpreter" "$?" 5
fi

echo ""
echo "sandbox_preload_landlock: $([ "$fail" -eq 0 ] && echo PASS || echo FAIL)"
[ "$fail" -eq 0 ]
