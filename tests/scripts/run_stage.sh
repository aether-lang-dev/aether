#!/bin/sh
# Run one stage of `make ci` under a time bound.
#
#   run_stage.sh <seconds> <stage name> <command> [args...]
#
# Every test in the .ae sweep has a bound of its own, but the stages around
# it did not: a hang in the C unit tests, the examples, the install or
# archive checks ran for as long as the job was allowed to (#2608). On the
# Windows runner, five such runs ended an hour in with "The hosted runner
# lost communication with the server" and no log at all.
#
# A stage that runs past its bound is ended (with everything it started)
# and fails as "[TIMEOUT] stage <name>", with the processes still running
# and their memory, so the run that hangs says where. The bounds are several
# times a healthy stage's time on the slowest runner.
set -u

secs="$1"
name="$2"
shift 2

if command -v timeout >/dev/null 2>&1; then
    TO="timeout -k 30 $secs"
elif command -v gtimeout >/dev/null 2>&1; then
    TO="gtimeout -k 30 $secs"
else
    TO=""
fi

$TO "$@"
rc=$?

# 124: ended at the bound. 137: killed 30 s later, when it ignored that.
if [ -n "$TO" ] && { [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; }; then
    echo ""
    echo "[TIMEOUT] stage '$name' ran past ${secs}s and was ended. Processes still running:"
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*)
            tasklist //FI "MEMUSAGE gt 50000" 2>/dev/null || ps -W ;;
        Darwin)
            ps -axo pid,rss,etime,command -m 2>/dev/null | head -20 ;;
        *)
            ps -eo pid,rss,etime,args --sort=-rss 2>/dev/null | head -20 ;;
    esac
    exit 1
fi
exit "$rc"
