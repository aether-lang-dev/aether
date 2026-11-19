#!/bin/sh
# A release tag that is not a version is refused before it reaches a shell or
# PowerShell command line (#3744).
#
# `ae install`, `ae upgrade` and `ae version install` build curl, wget, cp
# and rm command lines (PowerShell scripts on Windows) with the tag in them,
# and `ae upgrade` is sometimes run as root. The tag comes from GitHub's JSON
# or from the command line. The mock here (mock_github.py, mode badtag) names
# a newest release whose tag makes a file called "pwned" in the working
# directory if it reaches a POSIX shell inside double quotes or a PowerShell
# single-quoted string. Nothing touches the public internet: AE_GITHUB_API_BASE
# and AE_GITHUB_WEB_BASE point `ae` at the mock.
#
# Pinned properties:
#   1. `ae install` with no version refuses the latest release's tag, says
#      so, downloads nothing and runs nothing,
#   2. `ae upgrade` the same,
#   3. `ae version install <tag>` refuses a tag from the command line.
# HOME (and USERPROFILE) are redirected so the real ~/.aether is never touched.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"
[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] ae_release_tag_refused: ae not built"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "  [SKIP] ae_release_tag_refused: python3 needed"; exit 0; }
command -v curl >/dev/null 2>&1 || { echo "  [SKIP] ae_release_tag_refused: curl needed"; exit 0; }

TMP="$(mktemp -d)"
SRV_PID=""
cleanup() { stop_server "$SRV_PID"; rm -rf "$TMP"; return 0; }
trap cleanup EXIT
fail() {
    echo "  [FAIL] ae_release_tag_refused: $1"
    [ -n "$2" ] && [ -f "$2" ] && sed 's/^/        /' "$2"
    exit 1
}

mkdir -p "$TMP/rel" "$TMP/work" "$TMP/home/.aether"
HOME="$TMP/home"; export HOME
if command -v cygpath >/dev/null 2>&1; then
    USERPROFILE="$(cygpath -m "$TMP/home")"
else
    USERPROFILE="$TMP/home"
fi
export USERPROFILE

PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
BASE="http://127.0.0.1:$PORT"
: > "$TMP/rel/requests.log"
python3 "$SCRIPT_DIR/mock_github.py" "$PORT" "$TMP/rel" badtag >/dev/null 2>&1 &
SRV_PID=$!
i=0
until curl -s -o /dev/null "$BASE/" 2>/dev/null; do
    i=$((i + 1))
    [ "$i" -lt 50 ] || { echo "  [SKIP] ae_release_tag_refused: mock server did not start"; exit 0; }
    sleep 0.1
done

run_ae() {   # $1 = log name, the rest = ae's arguments
    log="$TMP/$1.log"
    shift
    ( cd "$TMP/work" && env -u GITHUB_TOKEN -u GH_TOKEN AE_GITHUB_WEB_BASE="$BASE" \
        AE_GITHUB_API_BASE="$BASE" "$AE" "$@" ) >"$log" 2>&1
}

# 1 ----------------------------------------------------------------------------
run_ae install install && fail "ae install accepted a tag that is not a version" "$TMP/install.log"
grep -q "not a version" "$TMP/install.log" || fail "ae install did not say the tag is not a version" "$TMP/install.log"
[ -e "$TMP/work/pwned" ] && fail "the tag ran as a command under ae install" "$TMP/install.log"
grep -q "releases/download" "$TMP/rel/requests.log" && fail "ae install downloaded with the refused tag" "$TMP/rel/requests.log"

# 2 ----------------------------------------------------------------------------
run_ae upgrade upgrade && fail "ae upgrade accepted a tag that is not a version" "$TMP/upgrade.log"
grep -q "not a version" "$TMP/upgrade.log" || fail "ae upgrade did not say the tag is not a version" "$TMP/upgrade.log"
[ -e "$TMP/work/pwned" ] && fail "the tag ran as a command under ae upgrade" "$TMP/upgrade.log"

# 3 ----------------------------------------------------------------------------
run_ae argv version install "v1\$(touch pwned)'+\$(ni pwned)+'" \
    && fail "ae version install accepted a version that is not one" "$TMP/argv.log"
grep -q "is not a version" "$TMP/argv.log" || fail "ae version install did not name the bad version" "$TMP/argv.log"
[ -e "$TMP/work/pwned" ] && fail "the argument ran as a command under ae version install" "$TMP/argv.log"
grep -q "releases/download" "$TMP/rel/requests.log" && fail "a refused version was downloaded" "$TMP/rel/requests.log"

echo "  [PASS] ae_release_tag_refused"
