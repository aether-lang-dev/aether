#!/bin/sh
# os.spawn_tree_to inside a sandboxed child: what its supervisor needs from
# the sandbox, and what the caller is told when the sandbox refuses it.
#
# spawn_tree_to forks a supervisor that does not exec, so it runs under the
# same preload and seccomp filter as the program that called it. It needs
#   - a new process: the kernel fence refuses a fork without grant_fork(), and
#     the spawn fails with "fork failed";
#   - to read /proc (the directory, and each process's stat) to find the
#     descendants that left the program's group, as a setsid'd browser helper
#     does. Without that read the supervisor would end the group and leave
#     the escaper running while the token read as if all had ended, so the
#     spawn is refused up front instead, naming the grant;
# and with both, the whole tree ends on tree_kill, the escaper included.
#
# The launcher starts itself as the sandboxed child (the pattern of
# sandbox_fork_grant). The child starts `sh -c 'setsid sleep 300 &; sleep
# 300'` as a tree and asks it to end; the launcher, uncontained, then checks
# by the escaper's recorded pid that it is gone (kill(pid, 0) inside the
# sandbox would not tell it), and ends it if it is not.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

case "$(uname -s 2>/dev/null)" in
    Linux) ;;
    *)
        echo "  [SKIP] sandbox_spawn_tree: the kernel fence and /proc are Linux's"
        exit 0 ;;
esac
if [ ! -x "$AE" ]; then
    echo "  [SKIP] sandbox_spawn_tree: ae not built"
    exit 0
fi
if [ ! -f "$ROOT/build/libaether_sandbox.so" ]; then
    echo "  [SKIP] sandbox_spawn_tree: build/libaether_sandbox.so not built"
    exit 0
fi
if ! command -v setsid >/dev/null 2>&1; then
    echo "  [SKIP] sandbox_spawn_tree: no setsid(1) to start the escaper with"
    exit 0
fi

TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT
export AETHER_CACHE_DIR="$TMP/cache"
. "$ROOT/tests/lib/sweep_timeout.sh"
sweep_timeout_bound TO "$ROOT" 90

cat > "$TMP/launch.ae" <<'AEEOF'
import std.os
import std.fs
import std.list
import std.sandbox
import std.string

alive(pid: int) -> bool {
    return os.kill(pid, 0) == 0
}

// The child: a tree whose program leaves a setsid'd sleeper, asked to end.
child(dir: string) {
    f = "${dir}/esc.pid"
    script = "setsid sleep 300 & echo $! > ${f}.tmp; mv ${f}.tmp ${f}; sleep 300"
    a = list.new()
    _a1 = list.add(a, "-c")
    _a2 = list.add(a, script)
    tree, err = os.spawn_tree_to("/bin/sh", a, null, "${dir}/tree.out", 500)
    list.free(a)
    if err != "" {
        println("child: tree refused: ${err}")
        return
    }
    println("child: tree started")
    esc = 0
    limit = os.now_monotonic_ms() + 5000
    while esc == 0 && os.now_monotonic_ms() < limit {
        text, e = fs.read(f)
        if e == "" {
            n, _x = string.to_int(string.trim(text))
            esc = n
        }
        sleep(20)
    }
    println("child: escaper pid known ${esc > 0}")
    _k = os.tree_kill(tree)
    code, _w = os.wait(os.tree_token(tree))
    println("child: token ${code}")
    os.tree_free(tree)
}

// launch <plain|noproc|proc> DIR: start this program again, held to grants
// that allow it everything the case names. `plain` reads /proc but has no grant_fork(),
// `noproc` has it and reads the system's libraries but not /proc, `proc`
// reads everything.
main() {
    if string.starts_with(os.args_get(1), "child=") == 1 {
        child(string.substring(os.args_get(1), 6, string.length(os.args_get(1))))
        return
    }
    mode = os.args_get(1)
    dir = os.args_get(2)
    g = sandbox.new("g") {
        grant_fs_read("/proc")
        grant_fs_read("/proc/*")
        grant_fs_read("${dir}/*")
        grant_fs_write("${dir}/*")
        grant_fs_read("/usr/*")
        grant_fs_read("/lib/*")
        grant_fs_read("/lib64/*")
        grant_fs_read("/bin/*")
        grant_fs_read("/etc/*")
        grant_fs_read("/dev/*")
        grant_exec("*")
        grant_env("*")
        grant_native("*")
    }
    defer sandbox.free(g)
    gf = sandbox.new("gf") {
        grant_fs_read("${dir}/*")
        grant_fs_write("${dir}/*")
        grant_fs_read("/usr/*")
        grant_fs_read("/lib/*")
        grant_fs_read("/lib64/*")
        grant_fs_read("/bin/*")
        grant_fs_read("/etc/*")
        grant_fs_read("/dev/*")
        grant_exec("*")
        grant_env("*")
        grant_native("*")
        grant_fork()
    }
    defer sandbox.free(gf)
    gp = sandbox.new("gp") {
        grant_fs_read("/*")
        grant_fs_write("/*")
        grant_exec("*")
        grant_env("*")
        grant_native("*")
        grant_fork()
    }
    defer sandbox.free(gp)
    grants = g
    if mode == "noproc" {
        grants = gf
    }
    if mode == "proc" {
        grants = gp
    }
    _z = fs.write("${dir}/esc.pid", "0")
    rc = spawn_sandboxed(grants, os.argv0(), "child=${dir}")
    println("rc=${rc}")
    text, e = fs.read("${dir}/esc.pid")
    if e == "" {
        n, _x = string.to_int(string.trim(text))
        if n > 0 {
            sleep(300)
            println("parent: escaper alive ${alive(n)}")
            if alive(n) {
                _k9 = os.kill(n, 9)
            }
        }
    }
}
AEEOF
if ! (cd "$TMP" && AETHER_HOME="$ROOT" "$AE" build launch.ae -o launch) > "$TMP/build.log" 2>&1; then
    echo "  [FAIL] sandbox_spawn_tree: the launcher does not build:"
    sed 's/^/        /' "$TMP/build.log" | head -12
    exit 1
fi

fail=0

# check LABEL MODE PATTERN...: run the launcher in MODE; each PATTERN (a
# basic regular expression) must match a whole line of its output.
check() {
    label="$1"
    mode="$2"
    shift 2
    mkdir -p "$TMP/$mode"
    out="$(cd "$TMP" && AETHER_HOME="$ROOT" $TO "$TMP/launch" "$mode" "$TMP/$mode" 2>&1)"
    for want in "$@"; do
        if ! printf '%s\n' "$out" | grep -q -- "^$want\$"; then
            echo "  [FAIL] $label: no line matching \"$want\" in:"
            printf '%s\n' "$out" | sed 's/^/        /' | head -12
            fail=1
            return
        fi
    done
    echo "  [PASS] $label"
}

check "without grant_fork: the spawn fails, nothing starts" plain \
    "child: tree refused: fork failed"
check "with grant_fork but no read of /proc: refused up front, nothing starts" noproc \
    "child: tree refused: cannot read /proc.*" "rc=0"
check "with both: the tree ends on tree_kill, the setsid'd escaper too" proc \
    "child: tree started" "child: escaper pid known true" "child: token 143" \
    "parent: escaper alive false"

exit $fail
