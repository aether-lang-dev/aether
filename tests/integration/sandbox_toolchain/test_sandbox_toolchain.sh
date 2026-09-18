#!/bin/sh
# Regression: a child invoked via spawn_sandboxed must be able to run the
# `ae`/`aetherc` -> `cc`/`gcc` -> `as`/`ld` toolchain.
#
# Was failing before the fix in runtime/libaether_sandbox_preload.c that
# removed the C-language vfork() wrapper: glibc's vfork() has a contract
# (the child must call only _exit/execve and the calling function must
# not return) that is violated by wrapping the symbol in a regular C
# function — the child returns through the wrapper frame on the parent's
# shared stack, corrupting it. gcc-on-Linux uses vfork() heavily, so a
# build inside spawn_sandboxed died with "Build failed" / rc=-1, and a
# real aeb orchestrator (the reporter) saw segfaults from the corrupted
# parent stack. Upstream report:
# sandbox-preload-toolchain-segfault.md.
#
# This test bakes in the property the fix establishes: a sandboxed child
# with enough grants can successfully run the toolchain to build and
# execute a trivial Aether program.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] sandbox preload is Linux-only (LD_PRELOAD)"
        exit 0 ;;
    Darwin)
        # spawn_sandboxed uses fork/shm_open/LD_PRELOAD — Linux-only.
        echo "  [SKIP] sandbox preload is Linux-only (LD_PRELOAD)"
        exit 0 ;;
esac

AE="$ROOT/build/ae"
PRELOAD_SRC="$ROOT/runtime/libaether_sandbox_preload.c"
if [ ! -x "$AE" ]; then
    echo "  [SKIP] $AE not built (run make first)"
    exit 0
fi
if [ ! -f "$PRELOAD_SRC" ]; then
    echo "  [FAIL] preload source missing: $PRELOAD_SRC"
    exit 1
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

# 1. Build the preload (matches what install.sh does for downstreams).
cc -shared -fPIC -o "$TMPDIR/libaether_sandbox.so" "$PRELOAD_SRC" -ldl -lrt 2>"$TMPDIR/preload-cc.log" || {
    echo "  [FAIL] preload .so build failed:"
    cat "$TMPDIR/preload-cc.log"
    exit 1
}

# 2. A trivial Aether program for the sandboxed child to compile.
cat > "$TMPDIR/inner.ae" <<'AE'
extern println(s: string)
main() { println("sandboxed-toolchain-ok") }
AE

# 3. The script the sandboxed child runs: invoke the full ae->gcc pipeline.
cat > "$TMPDIR/inner-build.sh" <<SH
#!/bin/sh
cd "$TMPDIR" || exit 71
rm -f inner.out
"$AE" build inner.ae -o inner.out 2>&1
ECODE=\$?
echo "ae-build-exit=\$ECODE"
# Absolute path required: the preload's exec grant pattern uses
# prefix-glob, and "/*" doesn't match a "./relative" execve target.
[ -x inner.out ] && "$TMPDIR/inner.out"
SH
chmod +x "$TMPDIR/inner-build.sh"

# 4. The launcher: spawn_sandboxed the script with generous grants so the
#    only thing being tested is whether the toolchain works under the
#    preload, not whether the grant set is sufficient.
cat > "$TMPDIR/launcher.ae" <<AE
import std.list
extern println(s: string)
main() {
    g = list.new()
    list.add(g, "fork");      list.add(g, "*")
    list.add(g, "exec");      list.add(g, "/*")
    list.add(g, "fs_read");   list.add(g, "/*")
    list.add(g, "fs_write");  list.add(g, "/*")
    list.add(g, "env");       list.add(g, "*")
    list.add(g, "native");    list.add(g, "*")
    rc = spawn_sandboxed(g, "sh", "$TMPDIR/inner-build.sh")
    println("rc=\${rc}")
}
AE

# 5. Build and run the launcher (outside the sandbox).
"$AE" build "$TMPDIR/launcher.ae" -o "$TMPDIR/launcher" >"$TMPDIR/launcher-build.log" 2>&1 || {
    echo "  [FAIL] launcher build failed:"
    tail -20 "$TMPDIR/launcher-build.log"
    exit 1
}

cd "$TMPDIR"
./launcher > out.log 2>&1 || {
    echo "  [FAIL] launcher exited non-zero:"
    cat out.log
    exit 1
}

# Assertions: the sandboxed child's ae build must have succeeded AND the
# produced binary must have run.
if ! grep -q '^ae-build-exit=0$' out.log; then
    echo "  [FAIL] sandboxed ae build did not exit 0:"
    cat out.log
    exit 1
fi
if ! grep -q '^sandboxed-toolchain-ok$' out.log; then
    echo "  [FAIL] sandboxed-built binary did not print the expected line:"
    cat out.log
    exit 1
fi
if ! grep -q '^rc=0$' out.log; then
    echo "  [FAIL] spawn_sandboxed returned non-zero:"
    cat out.log
    exit 1
fi

echo "  [PASS] toolchain (ae -> gcc -> cc1/as/ld) runs cleanly under spawn_sandboxed"
