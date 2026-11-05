#!/bin/sh
# Every way libc starts a program goes through the preload's exec and fork
# rules, not only execve and fork themselves.
#
# libc implements the rest of the exec family (execv, execvp, execvpe,
# execl, execlp, execle, fexecve, execveat), posix_spawn and posix_spawnp, system,
# popen and wordexp's command substitution on its own internal execve and
# clone or vfork, which the preload's execve and fork wrappers never see.
# Under a grant file with no exec and no fork grant, every one of them ran
# its program: only a direct execve was refused. So did os.execute and
# io.popen in a Lua run in-process (aether-guest-languages, lua/README.md).
#
# probe.c starts a program one way per mode; this runs it under grant sets
# that differ only in fork and exec, and holds the whole grid:
#   none     no fork, no exec: nothing starts
#   fork     fork only: a bare _Fork, nothing that runs a program
#   true     fork, and exec of true only: true runs every way, while what
#            needs /bin/sh (system, popen, wordexp's $(...), a script with
#            no #! line) is refused
#   script   fork, exec of the script only: execvp runs a file with no #!
#            line through /bin/sh, so it is refused for want of /bin/sh
#   all      fork, exec of everything: all of it runs, except a shell whose
#            environment has dropped the preload
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
case "$(uname -s 2>/dev/null)" in
    Linux|FreeBSD) ;;
    *)
        echo "  [SKIP] sandbox_spawn_family: the sandbox preload is Linux and FreeBSD only"
        exit 0 ;;
esac
CC_PROBE="${CC:-cc}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT

# The preload is built from source here, as sandbox_toolchain does, so the
# test holds the source it is run against.
libs=""
[ "$(uname -s)" = Linux ] && libs="-ldl -lrt"
# shellcheck disable=SC2086
if ! $CC_PROBE -shared -fPIC -o "$TMP/libaether_sandbox.so" \
        "$ROOT/runtime/libaether_sandbox_preload.c" $libs > "$TMP/cc.log" 2>&1; then
    echo "  [FAIL] sandbox_spawn_family: the preload does not build"
    sed 's/^/    /' "$TMP/cc.log"
    exit 1
fi
if ! $CC_PROBE -o "$TMP/probe" "$SCRIPT_DIR/probe.c" > "$TMP/cc.log" 2>&1; then
    echo "  [FAIL] sandbox_spawn_family: probe.c does not build"
    sed 's/^/    /' "$TMP/cc.log"
    exit 1
fi

TRUE_BIN=""
for t in /bin/true /usr/bin/true; do
    [ -x "$t" ] && { TRUE_BIN="$t"; break; }
done
[ -n "$TRUE_BIN" ] || { echo "  [SKIP] sandbox_spawn_family: no true binary"; exit 0; }
TRUE_REAL="$(cd "$(dirname "$TRUE_BIN")" && pwd -P)/$(basename "$TRUE_BIN")"
TRUE_DIR="$(dirname "$TRUE_BIN")"

SCRIPT="$TMP/noshebang"
printf 'exit 0\n' > "$SCRIPT"
chmod +x "$SCRIPT"

base='fs_read:/*
fs_write:/*
env:*
native:*'
printf '%s\n' "$base" > "$TMP/none"
printf '%s\nfork:*\n' "$base" > "$TMP/fork"
printf '%s\nfork:*\nexec:%s\nexec:%s\n' "$base" "$TRUE_BIN" "$TRUE_REAL" > "$TMP/true"
printf '%s\nfork:*\nexec:%s\n' "$base" "$SCRIPT" > "$TMP/script"
printf '%s\nfork:*\nexec:/*\n' "$base" > "$TMP/all"

# glibc's wordexp expands a plain word without a shell; FreeBSD's runs one
# for every call, so there it follows the shell's grants.
if [ "$(uname -s)" = Linux ]; then plain_noshell=RAN; else plain_noshell=refused; fi

fails=0
checked=0
expect() {   # expect <grants> <mode> <RAN|refused>
    got="$(cd "$TMP" && PATH="$TRUE_DIR" PROBE_TRUE="$TRUE_BIN" \
        AETHER_SANDBOX_LOG=none AETHER_SANDBOX_GRANTS="$TMP/$1" \
        LD_PRELOAD="$TMP/libaether_sandbox.so" "$TMP/probe" "$2" "$SCRIPT" 2>&1 | tail -1)"
    case "$got" in
        "$2: n/a") return 0 ;;
        "$2: $3") checked=$((checked + 1)) ;;
        *)
            echo "    grants '$1', $2: expected $3, got '$got'"
            fails=$((fails + 1)) ;;
    esac
}

STARTERS="posix_spawn posix_spawnp pidfd_spawn pidfd_spawnp"
EXECS="execve execv execvp execvpe execl execlp execle fexecve execveat execveat_fd"
SHELLS="system popen strip_system wordexp_cmd script_execvp"

for m in $STARTERS $EXECS $SHELLS _Fork; do expect none "$m" refused; done
expect none wordexp_plain "$plain_noshell"

for m in $STARTERS $EXECS $SHELLS; do expect fork "$m" refused; done
expect fork _Fork RAN
expect fork wordexp_plain "$plain_noshell"

for m in $STARTERS $EXECS _Fork; do expect true "$m" RAN; done
for m in $SHELLS; do expect true "$m" refused; done
expect true wordexp_plain "$plain_noshell"

expect script script_execvp refused

for m in $STARTERS $EXECS system popen wordexp_cmd script_execvp _Fork wordexp_plain; do
    expect all "$m" RAN
done
expect all strip_system refused

if [ "$fails" -ne 0 ]; then
    echo "  [FAIL] sandbox_spawn_family: $fails of $((fails + checked)) cells wrong"
    exit 1
fi
echo "  [PASS] sandbox_spawn_family: $checked cells, every way of starting a program held to fork and exec ($(uname -s))"
