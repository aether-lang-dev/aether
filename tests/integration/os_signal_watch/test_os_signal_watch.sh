#!/bin/sh
# os.signal_watch from outside the process (nib-code ask
# aether-signal-flag-and-tty-echo). The ask's repro: a program that loops on
# sleep and prints on the way out, sent SIGINT, died with 130 and never
# printed. Watching SIGINT and SIGTERM, it sees each, prints and exits 0;
# after signal_unwatch the default action is back and SIGTERM ends it.
# POSIX only: Windows has console events, not kill(1).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) echo "  [SKIP] os_signal_watch: no kill(1) signals on Windows"; exit 0 ;;
esac
[ -x "$AE" ] || { echo "  [SKIP] os_signal_watch: ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export AETHER_HOME="$ROOT"
fails=0

cat > "$TMP/wait.ae" <<'AE'
import std.os
import std.fs
import std.signal

main() {
    w, err = os.signal_watch([signal.SIGINT(), signal.SIGTERM()])
    if err != "" {
        println("watch failed: ${err}")
        return
    }
    ready = os.getenv("READY")
    fs.write(ready, "ready")
    i = 0
    while i < 400 {
        sig = os.signal_received(w)
        if sig != 0 {
            println("got ${sig}")
            os.signal_unwatch(w)
            return
        }
        sleep(25)
        i = i + 1
    }
    println("timed out")
}
AE

cat > "$TMP/restore.ae" <<'AE'
import std.os
import std.signal

extern raise(sig: int) -> int

main() {
    w, _ = os.signal_watch([signal.SIGTERM()])
    os.signal_unwatch(w)
    raise(signal.SIGTERM())
    println("survived SIGTERM after unwatch")
}
AE

if ! "$AE" build "$TMP/wait.ae" -o "$TMP/wait" >"$TMP/build.log" 2>&1 ||
   ! "$AE" build "$TMP/restore.ae" -o "$TMP/restore" >>"$TMP/build.log" 2>&1; then
    echo "  [FAIL] os_signal_watch: build failed"; sed 's/^/        /' "$TMP/build.log"; exit 1
fi

# send <signal name> <signal number>
send() {
    rm -f "$TMP/ready"
    READY="$TMP/ready" "$TMP/wait" >"$TMP/out" 2>&1 &
    pid=$!
    n=0
    while [ ! -f "$TMP/ready" ] && [ $n -lt 200 ]; do sleep 0.05; n=$((n + 1)); done
    # kill-ok: POSIX only (skipped on Windows); the signal is what is under test, sent to this test's own job
    kill -"$1" "$pid"
    wait "$pid"
    rc=$?
    out=$(cat "$TMP/out")
    if [ "$rc" -ne 0 ] || [ "$out" != "got $2" ]; then
        echo "  [FAIL] os_signal_watch: SIG$1 gave exit $rc and '$out', want 0 and 'got $2'"
        fails=$((fails + 1))
    fi
}
send INT 2
send TERM 15

# Through an inner shell whose stderr is dropped: the shell's own
# "Terminated" report is expected here, not output under test.
sh -c '"$1" >"$2" 2>&1' _ "$TMP/restore" "$TMP/rout" 2>/dev/null
rc=$?
if [ "$rc" -eq 0 ] || grep -q survived "$TMP/rout"; then
    echo "  [FAIL] os_signal_watch: SIGTERM after unwatch should end the process (exit $rc)"
    fails=$((fails + 1))
fi

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] os_signal_watch: SIGINT and SIGTERM are polled, the default is back after unwatch"
