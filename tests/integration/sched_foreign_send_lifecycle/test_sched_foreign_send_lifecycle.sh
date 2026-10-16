#!/bin/sh
# A send from a thread the scheduler does not own is refused while the
# scheduler is not running (#2912). Before, such a send could take a core's
# foreign-send lock between its destroy and its init when the scheduler was
# started again (a deleted critical section on Windows, EINVAL on POSIX),
# and after scheduler_cleanup() the send divided by a core count of 0.
# shim.c sends in a tight loop from its own thread while the scheduler goes
# through 25 shutdown / start cycles and a cleanup; each refused send is
# counted, and the program must end cleanly.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] sched_foreign_send_lifecycle: ae not built"; exit 0; }
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*|Windows_NT) EXE=".exe" ;; *) EXE="" ;; esac

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

if ! AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/probe.ae" -o "$TMPDIR/probe" \
        --extra "$SCRIPT_DIR/shim.c" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] sched_foreign_send_lifecycle: build failed"
    sed 's/^/    /' "$TMPDIR/build.log" | head -20
    exit 1
fi
if ! "$TMPDIR/probe$EXE" >"$TMPDIR/run.log" 2>&1 \
   || ! grep -q "sched_foreign_send_lifecycle passed" "$TMPDIR/run.log"; then
    echo "  [FAIL] sched_foreign_send_lifecycle: the probe failed"
    sed 's/^/    /' "$TMPDIR/run.log" | head -20
    exit 1
fi
grep -q "while the scheduler is not running; dropped (reported once)" "$TMPDIR/run.log" \
    || { echo "  [FAIL] sched_foreign_send_lifecycle: the refusal was not reported"; exit 1; }
echo "  [PASS] sched_foreign_send_lifecycle"
