#!/bin/bash
# `ae run <prog>.ae &` + `kill $!` must not orphan the program.
#
# ae run BUILDS then SPAWNS the binary and waits — it does not exec it,
# because it still has cleanup to do afterwards (evict a crashed binary
# from the cache, delete a non-cached temp exe). So killing the wrapper
# used to leave the child running, holding whatever socket it had bound.
#
# Ephemeral CI cannot catch this: the runner is discarded. On a persistent
# box the orphan squats the port and the NEXT run of the same test fails
# to bind — a green run poisoning the one after it with no code change in
# between. That is how it was found (aeci's proxtek VM, 2026-08-16).
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] ae_run_signal_forwarding: $AE not built"
    exit 0
fi
case "$(uname -s 2>/dev/null)" in
    Linux|Darwin) ;;
    *) echo "  [SKIP] ae_run_signal_forwarding: POSIX-only (kill \$! semantics)"; exit 0 ;;
esac
if ! command -v pgrep >/dev/null 2>&1; then
    echo "  [SKIP] ae_run_signal_forwarding: pgrep not available"
    exit 0
fi

TMPDIR_T="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_T"' EXIT

cat > "$TMPDIR_T/longrun.ae" <<'EOF'
extern sleep(ms: int)
main() {
    println("UP")
    i = 0
    while i < 120 { sleep(1000); i = i + 1 }
}
EOF

AETHER_HOME="$ROOT" "$AE" run "$TMPDIR_T/longrun.ae" > "$TMPDIR_T/out.log" 2>&1 &
WRAPPER=$!

# Wait for the program itself to be up, not merely for ae to have started:
# the build has to finish first.
CHILD=""
for _ in $(seq 1 60); do
    sleep 0.5
    CHILD="$(pgrep -P "$WRAPPER" 2>/dev/null | head -1)"
    [ -n "$CHILD" ] && break
done

if [ -z "$CHILD" ]; then
    # Nothing to assert against — the program never got far enough. Do not
    # fail: on a loaded or restricted box this is environmental.
    kill "$WRAPPER" 2>/dev/null
    echo "  [SKIP] ae_run_signal_forwarding: child never observed"
    exit 0
fi

kill "$WRAPPER" 2>/dev/null
sleep 2

if kill -0 "$CHILD" 2>/dev/null; then
    kill -9 "$CHILD" 2>/dev/null
    echo "  [FAIL] ae_run_signal_forwarding: child $CHILD survived the wrapper (orphaned)"
    exit 1
fi

echo "  [PASS] ae_run_signal_forwarding: killing ae run also terminated the program"
exit 0
