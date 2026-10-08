# Background servers a test script starts, addressed as jobs of this shell
# rather than by a remembered pid number (#2479).
#
# A pid number is only the child's while the child is unreaped. Once it
# exits and the shell reaps it, the number goes back to the pool, and
# MSYS2 hands pids out again out of order: a `kill -9 "$pid"` aimed at a
# server that died earlier (a lost port bind, a crash) lands on whatever
# process holds the number now. On the Windows runner that was another
# test's process or the sweep itself, and the job died with exit code 2304
# (SIGKILL) and no test named.
#
# A job is resolved by bash at the moment it signals: `kill %N` takes the
# job table's pid with child reaping blocked, so it reaches the child while
# it is alive and fails, harmlessly, once it is gone. The scripts must not
# `disown` their servers, which would drop them from the job table.
#
# Source this file, start each server with `cmd &`, keep `$!`, and use:
#   server_alive PID   0 while the server is still a running job
#   kill_server PID    SIGKILL it if it is still a job, then reap it

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

kill_server() {
    [ -n "$1" ] || return 0
    _sj=$(_server_job_number "$1")
    [ -n "$_sj" ] || return 0
    kill -9 "%$_sj" 2>/dev/null
    wait "%$_sj" 2>/dev/null
    return 0
}
