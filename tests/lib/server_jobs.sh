# Background programs a test script starts (servers, clients under a
# watchdog), ended through these helpers and never by `kill "$pid"` (#2479).
# tests/scripts/check_shell_kills.py holds every test to that.
#
# Why, on Windows. A `cmd &` that runs a native program (every Aether
# binary, `ae`, a MinGW openssl) puts an MSYS2 process in the job table that
# only stands in for the program. Any signal but 0 sent to that stand-in, by
# pid or as a job, makes the MSYS2 runtime (exit_process_tree, in
# winsup/cygwin/include/cygwin/exit_process.h) gather "the program's
# descendants" from the Windows process table by their RECORDED parent pid,
# and send the same signal to every MSYS2 process-group leader among them.
# Windows hands a pid out again as soon as its process is gone and never
# rewrites a recorded parent, so the program can carry the pid of a process
# that died long ago and is still on record as the parent of something
# alive. Every chain of parents on Windows ends in one (wininit's parent
# exits at boot, explorer's at logon), so the walk can come down through
# every native process to the cmd.exe that runs a CI step and to the step's
# own `bash -leo pipefail`, a process-group leader: the Windows job died
# with exit code 2304 (SIGKILL) and no test named, as a test ended its
# servers. The same walk, aimed on purpose, ends a cmd.exe ->
# `bash -leo pipefail` pair with exit code 2304.
#
# So on Windows the native program itself is ended, with TerminateProcess on
# its own pid (taskkill /F, never /T, which walks the same parent records),
# and its stand-in then exits by itself, unsignalled. Elsewhere the program
# gets a signal as a job: bash resolves `%N` while it still holds the child
# unreaped, so it never reaches a process that took over the pid of one that
# died. The scripts must not `disown` what they start.
#
# Source this file, start each program with `cmd &`, keep `$!`, and use:
#   server_alive PID       0 while the program is still a running job
#   kill_server PID        end it at once (SIGKILL elsewhere), then reap it
#   stop_server PID        SIGTERM, SIGKILL after 5 s, then reap it; on
#                          Windows the same as kill_server, as a native
#                          program there has no SIGTERM to handle
#   signal_server SIG PID  deliver SIG and nothing else, for a test whose
#                          subject is the signal; refused on Windows
# Each takes an empty PID, or one that is no longer a running job, as done.
# They need bash, which the sweep runs every test_*.sh with: `jobs -r` and a
# non-interactive `kill %N` are bash's (dash has neither).
#
# A program started through `ae run` is ae's child on Windows: ending ae
# there ends the program too (ae holds it in a job object), so keep `ae run`
# in front of a server only where that is what the test is about.

[ -n "${BASH_VERSION:-}" ] ||
    echo "tests/lib/server_jobs.sh: needs bash; run this test with bash" >&2

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) _SERVER_JOBS_WINDOWS=1 ;;
    *) _SERVER_JOBS_WINDOWS= ;;
esac

# The job number of the running background job whose pid is $1, or nothing.
# `-r` lists running jobs only; the state column is not matched, since bash
# prints it in the locale's language ("Running", "Ejecutando", ...).
_server_job_number() {
    jobs -rl 2>/dev/null | awk -v p="$1" '
        { n = $1; gsub(/[^0-9]/, "", n) }
        $2 == p { print n; exit }'
}

server_alive() {
    [ -n "$1" ] && [ -n "$(_server_job_number "$1")" ]
}

# Windows: end the process behind job pid $1 with TerminateProcess. While the
# stand-in lives, /proc/PID/winpid is the native program's own Windows pid
# and the stand-in holds a handle to it, so the number is still the
# program's. The image-name filter covers a program that exited by itself an
# instant ago: its pid could be anybody's by now, but not under its name.
_server_end_windows() {
    _sw=$(cat "/proc/$1/winpid" 2>/dev/null)
    _sx=$(cat "/proc/$1/winexename" 2>/dev/null)
    [ -n "$_sw" ] && [ -n "$_sx" ] || return 1
    _sx=${_sx##*\\}
    "$(cygpath -S)/taskkill.exe" //F //FI "PID eq $_sw" //FI "IMAGENAME eq $_sx" \
        >/dev/null 2>&1
}

# Wait up to $3 tenths of a second for job $2 (pid $1) to end, then reap it.
# 1 when it is still running.
_server_reap() {
    _si=0
    while [ "$_si" -lt "$3" ] && server_alive "$1"; do
        sleep 0.1
        _si=$((_si + 1))
    done
    server_alive "$1" && return 1
    # A reaped job's status is the program's: 143 or 137 when a signal ended
    # it. It is not the helper's answer, and a caller under `set -e` (most
    # tests) would exit with it, from its EXIT trap after its [PASS] line.
    wait "%$2" 2>/dev/null || :
    return 0
}

kill_server() {
    [ -n "$1" ] || return 0
    _sj=$(_server_job_number "$1")
    if [ -z "$_sj" ]; then
        wait "$1" 2>/dev/null || :   # status not ours: see _server_reap
        return 0
    fi
    # Each way of ending the job is an attempt, and fails when the program
    # ended by itself an instant ago (its /proc entry gone, nothing for
    # taskkill or kill to match). _server_reap says whether the job ended.
    # Under `set -e` the failed attempt ended the caller's test instead, with
    # no message (#3591).
    if [ -n "$_SERVER_JOBS_WINDOWS" ]; then
        _server_end_windows "$1" || :
    else
        kill -9 "%$_sj" 2>/dev/null || :
    fi
    _server_reap "$1" "$_sj" 100 2>/dev/null ||
        echo "server_jobs: job %$_sj (pid $1) did not end; left running" >&2
    return 0
}

stop_server() {
    [ -n "$1" ] || return 0
    if [ -n "$_SERVER_JOBS_WINDOWS" ]; then
        kill_server "$1"
        return 0
    fi
    _sj=$(_server_job_number "$1")
    if [ -z "$_sj" ]; then
        wait "$1" 2>/dev/null || :   # status not ours: see _server_reap
        return 0
    fi
    kill -TERM "%$_sj" 2>/dev/null || :   # an attempt, as in kill_server
    _server_reap "$1" "$_sj" 50 2>/dev/null && return 0
    kill_server "$1"
    return 0
}

signal_server() {
    [ -n "$2" ] || return 0
    if [ -n "$_SERVER_JOBS_WINDOWS" ]; then
        echo "server_jobs: signal_server $1: a signal to a native program on Windows also reaches unrelated processes (see tests/lib/server_jobs.sh)" >&2
        return 2
    fi
    _sj=$(_server_job_number "$2")
    [ -n "$_sj" ] || return 1
    kill -s "$1" "%$_sj"
}
