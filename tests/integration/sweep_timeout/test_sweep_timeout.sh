#!/bin/sh
# #2758: a hung test under the sweep's time limit fails as a timeout (124)
# and takes nothing else with it. The limit is tests/lib/sweep_timeout.sh,
# which tests/scripts/run_ae_test.sh, run_ae_sh_dir.sh and run_stage.sh use.
#
# On Windows the limit was timeout(1), which ends a hung test by signalling
# it. A signal sent to the MSYS2 process standing in for a native program
# makes the MSYS2 runtime signal every MSYS2 process-group leader it finds
# through recorded (and recycled) parent pids, and on the CI runner that
# reached the step's own bash: the job died with exit code 2304 (#2479). The
# limit there is now a job object ended by TerminateJobObject, which signals
# nothing.
#
# A deliberately hung native program runs under a 3 s limit twice over, in
# the foreground of a bash that traps SIGTERM and in the background beside
# it, inside a "sweep" shell that reports what it saw. The sweep shell has to
# survive and see 124, and both copies of the program have to be gone. On
# Windows the shell trap must not have run, since nothing may be signalled
# there. Elsewhere each native copy records the SIGTERM it receives directly;
# this does not depend on when a particular bash prints or runs its trap. The
# program gives up by itself after 60 s, so a limit that failed cannot leave
# it behind for long.
#
# #2772: where neither timeout(1) nor gtimeout exists (GitHub's macOS
# runners) the limit is sweep_timeout_posix, the same bound in perl; before
# it the sweep there had no limit at all. Every POSIX run checks that branch
# too (_SWEEP_TIMEOUT_TOOLS empty), and checks both bounds pass a command's
# own status through and kill one that ignores SIGTERM once KILL_AFTER is up.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) windows=1 ;;
    *) windows= ;;
esac
if [ -z "$windows" ] && ! command -v timeout >/dev/null 2>&1 &&
   ! command -v gtimeout >/dev/null 2>&1 && ! command -v perl >/dev/null 2>&1; then
    echo "  [SKIP] sweep_timeout: no timeout(1), gtimeout or perl here, so the sweep runs unbounded"
    exit 0
fi

cat > "$TMP/hang.c" <<'EOF'
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#define SELF ((unsigned long)GetCurrentProcessId())
#define NAP() Sleep(1000)
#else
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#define SELF ((unsigned long)getpid())
#define NAP() sleep(1)

static volatile sig_atomic_t term_fd = -1;

static void on_term(int signal) {
    static const char marker[] = "SIGTERM\n";
    int fd = (int)term_fd;
    if (fd >= 0) {
        (void)write(fd, marker, sizeof marker - 1);
        close(fd);
    }
    _exit(128 + signal);
}
#endif
int main(int argc, char** argv) {
#ifndef _WIN32
    if (argc > 2) {
        term_fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (term_fd < 0) return 2;
        struct sigaction action = {0};
        sigemptyset(&action.sa_mask);
        action.sa_handler = on_term;
        action.sa_flags = 0;
        if (sigaction(SIGTERM, &action, NULL) != 0) return 2;
    }
#endif
    FILE* f = argc > 1 ? fopen(argv[1], "a") : NULL;
    if (f) { fprintf(f, "%lu\n", SELF); fclose(f); }
    for (int i = 0; i < 60; i++) NAP();
    return 0;
}
EOF
# A name of its own, so on Windows the check below cannot mistake another
# program that took over the pid for this one.
hang="$TMP/sweep_hang_$$"
${CC:-cc} -o "$hang" "$TMP/hang.c" 2>"$TMP/cc.log" \
    || { cat "$TMP/cc.log"; fail "could not compile the hung program"; }
[ -f "$hang.exe" ] && hang="$hang.exe"

# Each copy writes its pid to a file of its own: two appends to one file can
# land on the same offset on Windows, and a pid would be lost.
cat > "$TMP/hung_test.sh" <<'EOF'
trap 'echo "SIGTERM reached the test"' TERM
"$1" "$2.background" "$3.background" &
"$1" "$2.foreground" "$3.foreground"
EOF

cat > "$TMP/sweep.sh" <<'EOF'
. "$1/tests/lib/sweep_timeout.sh"
sweep_timeout_bound TO "$1" 3
$TO bash "$2" "$3" "$4" "$5"
echo "rc=$?"
echo "sweep shell still running"
EOF

# check_hung LABEL TOOLS: the hung test under the bound sweep_timeout_bound
# picks with _SWEEP_TIMEOUT_TOOLS set to TOOLS ("default" leaves it unset).
check_hung() {
    label="$1"
    rm -f "$TMP/pid.background" "$TMP/pid.foreground" \
        "$TMP/term.background" "$TMP/term.foreground"
    start=$(date +%s)
    if [ "$2" = default ]; then
        out=$(bash "$TMP/sweep.sh" "$ROOT" "$TMP/hung_test.sh" "$hang" "$TMP/pid" "$TMP/term" 2>&1)
    else
        out=$(_SWEEP_TIMEOUT_TOOLS="$2" bash "$TMP/sweep.sh" "$ROOT" "$TMP/hung_test.sh" "$hang" "$TMP/pid" "$TMP/term" 2>&1)
    fi
    sweep_rc=$?
    elapsed=$(( $(date +%s) - start ))

    [ "$sweep_rc" -eq 0 ] || fail "$label: the sweep shell exited $sweep_rc: $out"
    printf '%s\n' "$out" | grep -q "sweep shell still running" \
        || fail "$label: the sweep shell did not get past the hung test: $out"
    printf '%s\n' "$out" | grep -q "^rc=124$" \
        || fail "$label: the hung test did not end as a timeout (124): $out"
    [ "$elapsed" -lt 30 ] \
        || fail "$label: the 3 s limit took ${elapsed}s: the program ran until it gave up by itself"

    cat "$TMP/pid.background" "$TMP/pid.foreground" 2>/dev/null | tr -d '\r' > "$TMP/pids.txt"
    npids=$(grep -c . "$TMP/pids.txt")
    [ "$npids" = 2 ] || fail "$label: expected both copies of the program to start, $npids did"
    for p in $(cat "$TMP/pids.txt"); do
        if [ -n "$windows" ]; then
            img=$(basename "$hang")
            "$(cygpath -S)/tasklist.exe" //NH //FI "PID eq $p" //FI "IMAGENAME eq $img" 2>/dev/null \
                | grep -qi "$img" && fail "$label: the hung program (pid $p) outlived the limit"
        else
            kill -0 "$p" 2>/dev/null && fail "$label: the hung program (pid $p) outlived the limit"
        fi
    done

    if [ -n "$windows" ]; then
        printf '%s\n' "$out" | grep -q "SIGTERM reached the test" \
            && fail "$label: the limit signalled the test on Windows, where a signal can reach unrelated processes (#2758)"
    else
        for copy in background foreground; do
            grep -qx "SIGTERM" "$TMP/term.$copy" 2>/dev/null \
                || fail "$label: SIGTERM never reached the $copy program: $out"
        done
    fi
}

# check_status LABEL TOOLS: a command's own status comes through the bound,
# and one that ignores SIGTERM is killed (137) once KILL_AFTER is up.
check_status() {
    label="$1"
    start=$(date +%s)
    if [ "$2" = default ]; then
        out=$(bash "$TMP/status.sh" "$ROOT" 2>&1)
    else
        out=$(_SWEEP_TIMEOUT_TOOLS="$2" bash "$TMP/status.sh" "$ROOT" 2>&1)
    fi
    elapsed=$(( $(date +%s) - start ))
    printf '%s\n' "$out" | grep -q "^exit=3$" \
        || fail "$label: a command's exit status did not come through the bound: $out"
    printf '%s\n' "$out" | grep -q "^signalled=137$" \
        || fail "$label: a command SIGKILL ended did not read as 137: $out"
    printf '%s\n' "$out" | grep -q "^stubborn=137$" \
        || fail "$label: a command that ignores SIGTERM was not killed after KILL_AFTER (137): $out"
    [ "$elapsed" -lt 20 ] \
        || fail "$label: the 2 s limit with a 1 s grace took ${elapsed}s"
}

cat > "$TMP/status.sh" <<'EOF'
. "$1/tests/lib/sweep_timeout.sh"
sweep_timeout_bound TO "$1" 30
$TO sh -c 'exit 3'
echo "exit=$?"
$TO sh -c 'kill -KILL $$'
echo "signalled=$?"
sweep_timeout_bound TO "$1" 2 1
$TO sh -c 'trap "" TERM; sleep 30'
echo "stubborn=$?"
EOF

if [ -n "$windows" ]; then
    check_hung "job object" default
    echo "  [PASS] sweep_timeout: a hung test ends as 124 through a job object, with no signal, and the sweep shell survives"
    exit 0
fi

if command -v timeout >/dev/null 2>&1 || command -v gtimeout >/dev/null 2>&1; then
    check_hung "timeout(1)" default
    check_status "timeout(1)" default
    via="timeout(1) and"
else
    via=""
fi
if command -v perl >/dev/null 2>&1; then
    check_hung "perl bound" ""
    check_status "perl bound" ""
    via="$via the perl bound (#2772)"
fi
echo "  [PASS] sweep_timeout: a hung test ends as 124 through ${via# }, its process group goes with it, statuses come through, and the sweep shell survives"
